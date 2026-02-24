/*--------------------------------------------------------------------------
 * sautils.c
 *	  Utility functions for the suffix array index access method.
 *
 *	  This file provides:
 *	  - Reloption parsing (max_prefix_len)
 *	  - SAState / meta page initialization and reading
 *	  - Page initialization for all section types
 *	  - Byte-wise comparison primitives for sort and search
 *	  - LCP computation
 *	  - Build phase progress reporting
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/backend/access/saindex/sautils.c
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/saindex_private.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/* ----------------------------------------------------------------
 *				Reloptions
 * ----------------------------------------------------------------
 *
 * The SA index exposes one reloption:
 *
 *   max_prefix_len (int, default 48):
 *       Maximum number of bytes stored per suffix in the SA leaf entries.
 *       Suffixes longer than this are truncated; queries longer than this
 *       require heap recheck.  Value of 48 yields 64-byte entries
 *       (one x86 cache line).
 *
 * We use dynamic reloption registration (add_reloption_kind) so that
 * the SA index can be added to the source tree without modifying the
 * global relopt_kind enum in reloptions.h.
 */

static relopt_kind sa_relopt_kind = 0;

/*
 * sa_register_reloptions
 *		Ensure SA reloptions are registered exactly once.
 *
 * Must be called before saoptions() or sahandler().  Safe to call
 * multiple times; second and subsequent calls are no-ops.
 */
void
sa_register_reloptions(void)
{
	if (sa_relopt_kind != 0)
		return;

	sa_relopt_kind = add_reloption_kind();
	add_int_reloption(sa_relopt_kind, "max_prefix_len",
					  "Maximum prefix length of suffixes stored in the index",
					  SA_DEFAULT_PREFIX_LEN,
					  SA_MIN_PREFIX_LEN,
					  SA_MAX_ALLOWED_PREFIX_LEN,
					  AccessExclusiveLock);
}

/*
 * saoptions
 *		Parse and validate reloptions for an SA index.
 *
 * Returns a palloc'd SAOptions struct, or NULL on validation failure
 * when validate is false.
 */
bytea *
saoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"max_prefix_len", RELOPT_TYPE_INT,
			offsetof(SAOptions, max_prefix_len)},
	};

	sa_register_reloptions();

	return (bytea *) build_reloptions(reloptions, validate,
									  sa_relopt_kind,
									  sizeof(SAOptions),
									  tab, lengthof(tab));
}


/* ----------------------------------------------------------------
 *				State initialization
 * ----------------------------------------------------------------
 */

/*
 * initSAState
 *		Initialize an SAState by reading the index's meta page.
 *
 * The caller must have at least AccessShareLock on the index.
 * On return, state contains cached copies of the key meta page fields
 * needed for all subsequent operations (entry size, entries per page, etc.).
 */
void
initSAState(SAState *state, Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	SAMetaPageData *meta;

	state->index = index;

	metabuf = ReadBuffer(index, SA_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta = SAPageGetMeta(metapage);

	if (meta->sa_magic != SA_INDEX_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("suffix array index \"%s\" has invalid magic number 0x%08X",
						RelationGetRelationName(index),
						meta->sa_magic)));

	if (meta->sa_version != SA_INDEX_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("suffix array index \"%s\" has unsupported version %d",
						RelationGetRelationName(index),
						meta->sa_version)));

	state->maxPrefixLen = meta->sa_max_prefix_len;
	state->entrySize = meta->sa_entry_size;
	state->entriesPerPage = meta->sa_entries_per_page;

	UnlockReleaseBuffer(metabuf);
}

/*
 * SAReadMeta
 *		Read a full copy of the meta page data into caller's struct.
 *
 * This is useful when the caller needs access to section descriptors
 * (block ranges, bitmap flags, etc.) beyond what SAState caches.
 */
void
SAReadMeta(Relation index, SAMetaPageData *metaOut)
{
	Buffer	buf;
	Page	page;

	buf = ReadBuffer(index, SA_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	memcpy(metaOut, SAPageGetMeta(page), sizeof(SAMetaPageData));

	UnlockReleaseBuffer(buf);

	if (metaOut->sa_magic != SA_INDEX_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("suffix array index has invalid magic number 0x%08X",
						metaOut->sa_magic)));
}


/* ----------------------------------------------------------------
 *				Page initialization
 * ----------------------------------------------------------------
 */

/*
 * SAInitPage
 *		Initialize an SA index page with the given flags.
 *
 * Sets up the standard page header, reserves the special space for
 * SAPageOpaqueData, and writes the page type flags.  The data area
 * between the page header and opaque data is zeroed by PageInit.
 *
 * Used for all page types: leaf, sufflen, tid, bitmap.
 */
void
SAInitPage(Page page, uint16 flags, Size pageSize)
{
	SAPageOpaque opaque;

	PageInit(page, pageSize, sizeof(SAPageOpaqueData));

	opaque = SAPageGetOpaque(page);
	opaque->sa_flags = flags;
	opaque->sa_next = InvalidBlockNumber;
	opaque->sa_unused = 0;
}

/*
 * SAInitMetaPage
 *		Initialize block 0 as the SA meta page.
 *
 * The caller provides a filled-in SAMetaPageData; this function copies
 * it into the page contents area and sets pd_lower to cover it (so that
 * WAL compression does not discard the meta data as free space).
 */
void
SAInitMetaPage(Page page, SAMetaPageData *metaIn, Size pageSize)
{
	SAMetaPageData *meta;

	/* Initialize page with opaque area */
	PageInit(page, pageSize, sizeof(SAPageOpaqueData));

	SAPageGetOpaque(page)->sa_flags = SA_PAGE_META;
	SAPageGetOpaque(page)->sa_next = InvalidBlockNumber;
	SAPageGetOpaque(page)->sa_unused = 0;

	/* Copy meta data into page contents area */
	meta = SAPageGetMeta(page);
	memcpy(meta, metaIn, sizeof(SAMetaPageData));

	/*
	 * Advance pd_lower past the meta data.  This tells the page manager
	 * (and WAL) that this space is in use, preventing xlog compression
	 * from zeroing it out.
	 */
	((PageHeader) page)->pd_lower =
		((char *) meta + sizeof(SAMetaPageData)) - (char *) page;
}

/*
 * SAInitEmptyMeta
 *		Fill an SAMetaPageData struct with valid defaults for an empty index.
 *
 * Used by sabuildempty() to create a valid but empty index, and as
 * the starting point for sabuild() before sections are allocated.
 */
void
SAInitEmptyMeta(SAMetaPageData *meta, int maxPrefixLen)
{
	MemSet(meta, 0, sizeof(SAMetaPageData));

	meta->sa_magic = SA_INDEX_MAGIC;
	meta->sa_version = SA_INDEX_VERSION;

	meta->sa_max_prefix_len = maxPrefixLen;
	meta->sa_entry_size = SA_ENTRY_SIZE(maxPrefixLen);
	meta->sa_entries_per_page = SAEntriesPerPage(meta->sa_entry_size);

	meta->sa_num_entries = 0;
	meta->sa_num_heap_tuples = 0;

	/* All section pointers invalid — no data pages allocated */
	meta->sa_leaf_start = InvalidBlockNumber;
	meta->sa_leaf_pages = 0;

	meta->sa_sufflen_start = InvalidBlockNumber;
	meta->sa_sufflen_pages = 0;

	meta->sa_tid_start = InvalidBlockNumber;
	meta->sa_tid_pages = 0;

	meta->sa_bitmap_start = InvalidBlockNumber;
	meta->sa_bitmap_pages = 0;
	meta->sa_bitmap_flags = 0;
	meta->sa_bitmap_pad = 0;
}


/* ----------------------------------------------------------------
 *				Comparison functions
 * ----------------------------------------------------------------
 *
 * All comparisons use byte-wise (memcmp) ordering.
 *
 * Why not collation-aware?  Suffix array substring search requires that
 * if string S contains substring P starting at offset k, then the suffix
 * S[k..] shares a prefix with P.  This prefix-containment property holds
 * under byte-wise ordering but NOT under arbitrary locale collations
 * (where "a" < "b" does not imply "aX" < "bX" for all X).
 *
 * Consequence: SA indexes are only usable with deterministic (byte-wise)
 * collations or C/POSIX locale.  The opclass should enforce this.
 */

/*
 * sa_compare_suffixes
 *		Compare two full suffix byte sequences for sort ordering.
 *
 * This is the comparison used during bulk build to sort ALL suffixes
 * by their complete text.  The sort is exact — no truncation is applied.
 * On equal prefix, the shorter suffix sorts first.
 *
 * This establishes the canonical suffix array order that is then
 * written to disk (with key truncation at write time).
 */
int
sa_compare_suffixes(const char *a, int alen, const char *b, int blen)
{
	int		cmplen = Min(alen, blen);
	int		result;

	result = memcmp(a, b, cmplen);
	if (result != 0)
		return result;

	/* Shorter suffix sorts first */
	if (alen != blen)
		return (alen < blen) ? -1 : 1;

	return 0;
}

/*
 * sa_compare_prefix
 *		Compare a search pattern against a stored entry key.
 *
 * entry_key points to sa_key in an SAEntry (max_prefix_len bytes,
 * zero-padded).  pattern/pattern_len is the query string.
 *
 * Compares the first min(pattern_len, max_prefix_len) bytes.
 * Returns:
 *   < 0  if entry's key sorts before the pattern
 *     0  if entry's key prefix-matches the pattern
 *   > 0  if entry's key sorts after the pattern
 *
 * This is the comparison used in binary search.  A return of 0 means
 * the entry is within the matching range.  If pattern_len > max_prefix_len,
 * the match is potentially lossy and the caller must heap-recheck.
 */
int
sa_compare_prefix(const char *entry_key, int max_prefix_len,
				  const char *pattern, int pattern_len)
{
	int		cmplen = Min(pattern_len, max_prefix_len);

	return memcmp(entry_key, pattern, cmplen);
}

/*
 * sa_compare_keys
 *		General key comparison with prefix truncation.
 *
 * Compares keys a and b as they would appear in the stored SA:
 * each is effectively truncated to max_prefix_len bytes, then
 * compared.  On equal truncated prefix, shorter (truncated) length
 * sorts first.
 *
 * Useful for verifying SA ordering invariants and in any context
 * where two already-stored keys need to be compared.
 */
int
sa_compare_keys(const char *a, int alen,
				const char *b, int blen,
				int max_prefix_len)
{
	int		alen_eff = Min(alen, max_prefix_len);
	int		blen_eff = Min(blen, max_prefix_len);
	int		cmplen = Min(alen_eff, blen_eff);
	int		result;

	result = memcmp(a, b, cmplen);
	if (result != 0)
		return result;

	if (alen_eff != blen_eff)
		return (alen_eff < blen_eff) ? -1 : 1;

	return 0;
}

/*
 * sa_compute_lcp
 *		Compute the longest common prefix of two byte sequences.
 *
 * Returns the number of leading bytes that are identical, bounded by
 * max_prefix_len.  This bound exists because the SA only stores
 * max_prefix_len bytes per key, so LCP values beyond that are not
 * representable and not useful for LCP-accelerated search.
 *
 * Called during build to populate SAEntry.sa_lcp for each entry,
 * by comparing the current suffix against the previous one in
 * sorted order.
 */
int
sa_compute_lcp(const char *a, int alen, const char *b, int blen,
			   int max_prefix_len)
{
	int		maxlen = Min(Min(alen, blen), max_prefix_len);
	int		i;

	for (i = 0; i < maxlen; i++)
	{
		if (a[i] != b[i])
			break;
	}

	return i;
}


/* ----------------------------------------------------------------
 *				Progress reporting
 * ----------------------------------------------------------------
 */

/*
 * sabuildphasename
 *		Return a human-readable name for a build phase number.
 *
 * Used by the progress reporting infrastructure to display the
 * current build phase in pg_stat_progress_create_index.
 */
char *
sabuildphasename(int64 phasenum)
{
	switch (phasenum)
	{
		case SA_BUILD_PHASE_GENERATE:
			return "generating suffix entries";
		case SA_BUILD_PHASE_SORT:
			return "sorting suffix entries";
		case SA_BUILD_PHASE_WRITE_SA:
			return "writing SA leaf pages";
		case SA_BUILD_PHASE_WRITE_AUX:
			return "writing auxiliary arrays";
		default:
			return NULL;
	}
}

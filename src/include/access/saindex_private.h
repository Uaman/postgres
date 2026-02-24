/*--------------------------------------------------------------------------
 * saindex_private.h
 *	  Header for PostgreSQL suffix array index access method implementation.
 *
 *	A suffix array index stores all suffixes of indexed text values in
 *	sorted order, enabling efficient substring search (LIKE '%x%'),
 *	prefix search (LIKE 'x%'), and suffix search (LIKE '%x').
 *
 *	The index uses byte-wise (memcmp) ordering rather than collation-aware
 *	ordering.  This is correct for substring search because pattern matching
 *	operates at the byte level.
 *
 *	On-disk layout (contiguous sections, flat arrays):
 *
 *	  Block 0:                          Meta page
 *	  Blocks [leaf_start  .. +leaf_pages):    SA leaf pages (sorted SAEntry)
 *	  Blocks [sufflen_start.. +sufflen_pages): Suffix-length array (uint16)
 *	  Blocks [tid_start   .. +tid_pages):     TID array (ItemPointerData)
 *	  Blocks [bitmap_start.. +bitmap_pages):  Bitmaps (packed bits)
 *
 *	All sections use flat array layout with O(1) global-index-to-page
 *	mapping:
 *	  page_no = section_start + (i / entries_per_page)
 *	  offset  = i % entries_per_page
 *
 *	This property requires fixed-size entries and is architecturally
 *	necessary for true suffix array binary search.
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/include/access/saindex_private.h
 *--------------------------------------------------------------------------
 */
#ifndef SAINDEX_PRIVATE_H
#define SAINDEX_PRIVATE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "utils/tuplesort.h"


/* ----------------------------------------------------------------
 *					Constants
 * ----------------------------------------------------------------
 */

#define SA_INDEX_MAGIC			0x53414658	/* "SAFX" */
#define SA_INDEX_VERSION		1

/*
 * Fixed-location pages.
 * Meta page is always block 0.  SA leaf pages begin at block 1.
 */
#define SA_METAPAGE_BLKNO		0
#define SA_FIRST_LEAF_BLKNO		1

/*
 * Operator strategy numbers for the SA opclass.
 *
 * These map to the operators exposed by the opclass:
 *	 contains (%x%)  -  find all rows whose value contains pattern as substring
 *	 prefix   (x%)   -  find all rows whose value starts with pattern
 *	 suffix   (%x)   -  find all rows whose value ends with pattern
 */
#define SA_STRATEGY_CONTAINS	1		/* LIKE '%x%' */
#define SA_STRATEGY_PREFIX		2		/* LIKE 'x%'  */
#define SA_STRATEGY_SUFFIX		3		/* LIKE '%x'  */

/*
 * Support function numbers.
 */
#define SA_COMPARE_PROC			1		/* byte-wise key comparison */

/*
 * Prefix length limits.
 *
 * SA_DEFAULT_PREFIX_LEN of 48 yields 64-byte entries (one x86 cache line):
 *	 16 bytes fixed fields + 48 bytes key = 64 bytes per entry.
 */
#define SA_DEFAULT_PREFIX_LEN		48
#define SA_MIN_PREFIX_LEN			8
#define SA_MAX_ALLOWED_PREFIX_LEN	256

/*
 * Build phase numbers for progress reporting via
 * pgstat_progress_update_param().
 */
#define SA_BUILD_PHASE_GENERATE		1	/* generating suffix entries from heap */
#define SA_BUILD_PHASE_SORT			2	/* external sort of all entries */
#define SA_BUILD_PHASE_WRITE_SA		3	/* writing sorted SA leaf pages + LCP */
#define SA_BUILD_PHASE_WRITE_AUX	4	/* writing auxiliary arrays & bitmaps */


/* ----------------------------------------------------------------
 *				On-disk page structures
 * ----------------------------------------------------------------
 */

/*
 * SAPageOpaqueData -- stored in the special space at the end of every
 * SA index page (except the meta page, which uses SAMetaPageData in
 * the page contents area).
 *
 * sa_flags identifies the page type.  All page types share this opaque
 * structure for uniformity, even though not all fields are meaningful
 * for every page type.
 */
typedef struct SAPageOpaqueData
{
	BlockNumber sa_next;		/* next block in this section, or
								 * InvalidBlockNumber if last */
	uint16		sa_flags;		/* page type flags, see SA_PAGE_* below */
	uint16		sa_unused;		/* reserved for future use */
} SAPageOpaqueData;

typedef SAPageOpaqueData *SAPageOpaque;

/* Page flags */
#define SA_PAGE_META		(1 << 0)
#define SA_PAGE_LEAF		(1 << 1)	/* SA leaf page (sorted entries) */
#define SA_PAGE_SUFFLEN		(1 << 2)	/* suffix-length array page */
#define SA_PAGE_TID			(1 << 3)	/* TID array page */
#define SA_PAGE_BITMAP		(1 << 4)	/* bitmap page */
#define SA_PAGE_DELETED		(1 << 5)	/* page has been recycled */


/* ----------------------------------------------------------------
 *				SA leaf entry (the core suffix array element)
 * ----------------------------------------------------------------
 *
 * Each SAEntry represents one suffix of one indexed text value.
 * Entries are stored in lexicographic (memcmp) order across leaf pages.
 *
 * The sa_key field is a fixed-length byte array whose size is determined
 * by the index's max_prefix_len reloption (stored in meta page).
 * Suffixes shorter than max_prefix_len are zero-padded.
 * Suffixes longer than max_prefix_len are truncated; queries longer
 * than max_prefix_len require heap recheck.
 *
 * Layout is arranged so that all fields are naturally aligned when
 * max_prefix_len is a multiple of 4:
 *
 *	 Offset  Field          Size   Notes
 *	 ------  -----          ----   -----
 *	 0       sa_lcp         4      LCP with previous entry in sort order
 *	 4       sa_offset      4      byte offset of suffix start in original value
 *	 8       sa_heaptid     6      heap row pointer (block + offset)
 *	 14      sa_suffixlen   2      true suffix length (may exceed max_prefix_len)
 *	 16      sa_key[..]     N      suffix text, zero-padded to max_prefix_len
 *	 ------
 *	 Total: 16 + max_prefix_len bytes per entry
 *
 * With default max_prefix_len=48: entry = 64 bytes = one cache line.
 */
typedef struct SAEntry
{
	int32			sa_lcp;			/* LCP length with SA[i-1]; 0 for first */
	int32			sa_offset;		/* byte position of suffix in original value */
	ItemPointerData sa_heaptid;		/* pointer to heap tuple */
	uint16			sa_suffixlen;	/* actual suffix length before truncation */
	char			sa_key[FLEXIBLE_ARRAY_MEMBER];	/* suffix text (fixed-len) */
} SAEntry;

/*
 * Size computations for SAEntry.
 *
 * SA_ENTRY_FIXED_SIZE is the size of everything before sa_key.
 * SA_ENTRY_SIZE(pfxlen) is the total size of one entry including key.
 */
#define SA_ENTRY_FIXED_SIZE		((int) offsetof(SAEntry, sa_key))
#define SA_ENTRY_SIZE(pfxlen)	(SA_ENTRY_FIXED_SIZE + (pfxlen))

/*
 * An entry's suffix was not truncated if its true length fits within
 * the stored prefix.  In that case, comparison is exact and no heap
 * recheck is needed for the key match.
 */
#define SAEntryIsExact(entry, pfxlen) \
	((entry)->sa_suffixlen <= (pfxlen))

/*
 * An entry represents the full original value (not a proper suffix)
 * when the suffix starts at offset 0.  Used for prefix queries (x%).
 */
#define SAEntryIsFullValue(entry) \
	((entry)->sa_offset == 0)

/*
 * Reconstruct original value length from suffix metadata.
 */
#define SAEntryOrigLen(entry) \
	((entry)->sa_offset + (entry)->sa_suffixlen)


/* ----------------------------------------------------------------
 *				Meta page
 * ----------------------------------------------------------------
 *
 * The meta page is always block 0.  It is stored in the page contents
 * area (via PageGetContents), not as a regular tuple.
 *
 * It contains all information needed to locate and interpret every
 * section of the index.
 */
typedef struct SAMetaPageData
{
	/* Identity */
	uint32		sa_magic;			/* SA_INDEX_MAGIC */
	uint32		sa_version;			/* SA_INDEX_VERSION */

	/* Index parameters (immutable after build) */
	int32		sa_max_prefix_len;	/* prefix length used to build this index */
	int32		sa_entry_size;		/* SA_ENTRY_SIZE(sa_max_prefix_len), cached */
	int32		sa_entries_per_page; /* entries per SA leaf page, cached */

	/* Global counts */
	int64		sa_num_entries;		/* total number of suffix entries */
	int64		sa_num_heap_tuples;	/* number of indexed heap rows */

	/* ------ Section descriptors ------ */

	/*
	 * SA leaf section: sorted SAEntry records.
	 * The core suffix array.  Binary search operates on this section.
	 */
	BlockNumber sa_leaf_start;		/* first SA leaf block */
	int32		sa_leaf_pages;		/* number of SA leaf pages */

	/*
	 * Suffix-length array: packed uint16 values, one per SA entry.
	 * Used for dense filtering in suffix queries (LIKE '%x').
	 * sa_sufflen[i] == SA[i].sa_suffixlen.
	 */
	BlockNumber sa_sufflen_start;
	int32		sa_sufflen_pages;

	/*
	 * TID array: packed ItemPointerData values, one per SA entry.
	 * Used for dense TID collection without reading full SA entries.
	 * sa_tid[i] == SA[i].sa_heaptid.
	 */
	BlockNumber sa_tid_start;
	int32		sa_tid_pages;

	/*
	 * Bitmap section: packed bits, one bit per SA entry.
	 * Multiple bitmaps are stored sequentially within this section.
	 * sa_bitmap_flags indicates which bitmaps are present.
	 */
	BlockNumber sa_bitmap_start;
	int32		sa_bitmap_pages;
	uint16		sa_bitmap_flags;	/* which bitmaps were built */
	uint16		sa_bitmap_pad;		/* alignment padding */
} SAMetaPageData;

/*
 * Bitmap type flags for sa_bitmap_flags.
 * Each flag adds one complete bitmap (sa_num_entries bits) to the
 * bitmap section, in the order of flag bit position.
 */
#define SA_BITMAP_OFFSET_ZERO		(1 << 0)
		/* Bit i = 1 iff SA[i].sa_offset == 0.
		 * Used for prefix queries (LIKE 'x%'): after binary search locates
		 * the range of suffixes starting with the pattern, this bitmap filters
		 * to entries where the suffix IS the full value (starts at offset 0). */

#define SA_BITMAP_EXACT_SUFFIX		(1 << 1)
		/* Bit i = 1 iff SA[i].sa_suffixlen <= sa_max_prefix_len.
		 * The stored prefix captures the full suffix — comparison is exact,
		 * no heap recheck is needed for key verification. */

/*
 * Number of bitmaps present, computed from flags.
 */
#define SABitmapCount(flags) \
	(((flags) & SA_BITMAP_OFFSET_ZERO  ? 1 : 0) + \
	 ((flags) & SA_BITMAP_EXACT_SUFFIX ? 1 : 0))

/*
 * Ordinal position of a specific bitmap within the bitmap section.
 * Bitmaps are stored in flag bit order:
 *   position 0: offset-zero  (if present)
 *   position 1: exact-suffix (if present, and offset-zero is also present)
 *   etc.
 */
#define SABitmapOrdinal(flags, which) \
	(__builtin_popcount((flags) & ((which) - 1)))


/* ----------------------------------------------------------------
 *				Page access macros
 * ----------------------------------------------------------------
 */

/* Get opaque data from any SA index page */
#define SAPageGetOpaque(page) \
	((SAPageOpaque) PageGetSpecialPointer(page))

/* Get meta page data from meta page */
#define SAPageGetMeta(page) \
	((SAMetaPageData *) PageGetContents(page))

/* Page type predicates */
#define SAPageIsLeaf(page) \
	((SAPageGetOpaque(page)->sa_flags & SA_PAGE_LEAF) != 0)
#define SAPageIsSufflen(page) \
	((SAPageGetOpaque(page)->sa_flags & SA_PAGE_SUFFLEN) != 0)
#define SAPageIsTid(page) \
	((SAPageGetOpaque(page)->sa_flags & SA_PAGE_TID) != 0)
#define SAPageIsBitmap(page) \
	((SAPageGetOpaque(page)->sa_flags & SA_PAGE_BITMAP) != 0)
#define SAPageIsDeleted(page) \
	((SAPageGetOpaque(page)->sa_flags & SA_PAGE_DELETED) != 0)

/*
 * Pointer to the data area of a page (just past the page header).
 * All section page types store their array data starting here.
 */
#define SAPageGetData(page) \
	((Pointer) PageGetContents(page))

/*
 * Usable data bytes per page, excluding page header and special space.
 */
#define SA_PAGE_DATA_SIZE \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
	 MAXALIGN(sizeof(SAPageOpaqueData)))


/* ----------------------------------------------------------------
 *				SA leaf section access macros
 * ----------------------------------------------------------------
 *
 * SA leaf pages store fixed-size SAEntry records contiguously.
 * Global SA index i maps to a specific page and offset:
 *
 *	 blkno  = meta->sa_leaf_start + (i / meta->sa_entries_per_page)
 *	 offset = i % meta->sa_entries_per_page
 *	 entry  = (SAEntry *)(page_data + offset * meta->sa_entry_size)
 */

/* Number of SAEntry records that fit on one leaf page */
#define SAEntriesPerPage(entry_size) \
	((int) (SA_PAGE_DATA_SIZE / (entry_size)))

/* Get pointer to the j-th entry on a leaf page */
#define SAPageGetEntry(page, j, entry_size) \
	((SAEntry *) (SAPageGetData(page) + (j) * (entry_size)))


/* ----------------------------------------------------------------
 *				Suffix-length array access macros
 * ----------------------------------------------------------------
 *
 * Packed uint16 values, one per SA entry.
 * 8152 / 2 = 4076 values per page.
 */

#define SA_SUFFLENS_PER_PAGE \
	((int) (SA_PAGE_DATA_SIZE / sizeof(uint16)))

/* Locate sufflen[i] across pages */
#define SASufflenBlkno(meta, i) \
	((meta)->sa_sufflen_start + ((i) / SA_SUFFLENS_PER_PAGE))

#define SASufflenOffset(i) \
	((i) % SA_SUFFLENS_PER_PAGE)

/* Get pointer to array of uint16 on a sufflen page */
#define SAPageGetSufflens(page) \
	((uint16 *) SAPageGetData(page))


/* ----------------------------------------------------------------
 *				TID array access macros
 * ----------------------------------------------------------------
 *
 * Packed ItemPointerData values (6 bytes each), one per SA entry.
 * 8152 / 6 = 1358 values per page.
 */

#define SA_TIDS_PER_PAGE \
	((int) (SA_PAGE_DATA_SIZE / sizeof(ItemPointerData)))

/* Locate tid[i] across pages */
#define SATidBlkno(meta, i) \
	((meta)->sa_tid_start + ((i) / SA_TIDS_PER_PAGE))

#define SATidOffset(i) \
	((i) % SA_TIDS_PER_PAGE)

/* Get pointer to array of ItemPointerData on a TID page */
#define SAPageGetTids(page) \
	((ItemPointerData *) SAPageGetData(page))


/* ----------------------------------------------------------------
 *				Bitmap access macros
 * ----------------------------------------------------------------
 *
 * Each bitmap is sa_num_entries bits, packed 8 per byte.
 * 8152 * 8 = 65216 bits per page.
 *
 * Multiple bitmaps are stored sequentially: all pages for bitmap 0,
 * then all pages for bitmap 1, etc.
 */

#define SA_BITMAP_BITS_PER_PAGE \
	((int64) (SA_PAGE_DATA_SIZE * BITS_PER_BYTE))

/* Number of pages needed for one bitmap of N entries */
#define SABitmapPagesForEntries(n) \
	((int32) (((n) + SA_BITMAP_BITS_PER_PAGE - 1) / SA_BITMAP_BITS_PER_PAGE))

/*
 * Locate the page and byte/bit offset for bit i within a single bitmap
 * that starts at base_blkno.
 */
#define SABitmapBlkno(base_blkno, i) \
	((base_blkno) + (BlockNumber) ((i) / SA_BITMAP_BITS_PER_PAGE))

#define SABitmapByteOffset(i) \
	(((i) % SA_BITMAP_BITS_PER_PAGE) / BITS_PER_BYTE)

#define SABitmapBitOffset(i) \
	(((i) % SA_BITMAP_BITS_PER_PAGE) % BITS_PER_BYTE)

/*
 * Compute the base block of a specific bitmap type within the bitmap section.
 * Bitmaps are stored in flag-bit order.
 */
#define SABitmapBaseBlkno(meta, which) \
	((meta)->sa_bitmap_start + \
	 SABitmapOrdinal((meta)->sa_bitmap_flags, (which)) * \
	 SABitmapPagesForEntries((meta)->sa_num_entries))

/* Get pointer to raw bitmap bytes on a bitmap page */
#define SAPageGetBitmapData(page) \
	((uint8 *) SAPageGetData(page))

/*
 * Test/set/clear a single bit on an in-memory bitmap page.
 */
#define SABitmapPageTestBit(page, byteoff, bitoff) \
	((SAPageGetBitmapData(page)[(byteoff)] >> (bitoff)) & 1)

#define SABitmapPageSetBit(page, byteoff, bitoff) \
	(SAPageGetBitmapData(page)[(byteoff)] |= (1 << (bitoff)))

#define SABitmapPageClearBit(page, byteoff, bitoff) \
	(SAPageGetBitmapData(page)[(byteoff)] &= ~(1 << (bitoff)))


/* ----------------------------------------------------------------
 *				Reloptions
 * ----------------------------------------------------------------
 */

typedef struct SAOptions
{
	int32		vl_len_;			/* varlena header (do not touch directly!) */
	int			max_prefix_len;		/* suffix truncation length */
} SAOptions;

#define SAGetMaxPrefixLen(relation) \
	((relation)->rd_options ? \
	 ((SAOptions *) (relation)->rd_options)->max_prefix_len : \
	 SA_DEFAULT_PREFIX_LEN)


/* ----------------------------------------------------------------
 *				In-memory working state
 * ----------------------------------------------------------------
 */

/*
 * SAState: runtime descriptor for an open SA index.
 * Initialized once per index operation (build, scan, vacuum) and
 * provides cached copies of meta page fields plus support function info.
 */
typedef struct SAState
{
	Relation	index;				/* the index relation */
	int32		maxPrefixLen;		/* sa_max_prefix_len from meta */
	int32		entrySize;			/* SA_ENTRY_SIZE(maxPrefixLen) */
	int32		entriesPerPage;		/* SAEntriesPerPage(entrySize) */
} SAState;


/* ----------------------------------------------------------------
 *				Scan state
 * ----------------------------------------------------------------
 *
 * SAScanOpaqueData is attached to IndexScanDesc->opaque.
 * It tracks the current position within the SA during a scan.
 */
typedef struct SAScanOpaqueData
{
	SAState		sastate;

	MemoryContext tempCtx;			/* per-scan temp memory */

	/* Cached meta page values */
	int64		numEntries;			/* total SA entries */
	SAMetaPageData metaCopy;		/* snapshot of meta page */

	/* Query info */
	StrategyNumber strategy;		/* SA_STRATEGY_* */
	char	   *queryPattern;		/* search pattern (null-terminated) */
	int			queryLen;			/* strlen(queryPattern) */

	/*
	 * Binary search result: the range [lo, hi) in global SA indices
	 * where all entries have sa_key starting with queryPattern.
	 * Set by initial binary search; used by subsequent scan steps.
	 */
	bool		rangeValid;			/* has binary search been performed? */
	int64		rangeLo;			/* first matching SA index (inclusive) */
	int64		rangeHi;			/* last matching SA index (exclusive) */

	/* Current scan position within [rangeLo, rangeHi) */
	int64		curPos;				/* next SA index to examine */
	bool		scanDone;			/* all results returned? */
} SAScanOpaqueData;

typedef SAScanOpaqueData *SAScanOpaque;


/* ----------------------------------------------------------------
 *				Build state
 * ----------------------------------------------------------------
 */

typedef struct SABuildState
{
	SAState		sastate;

	/* Sort accumulator for all suffix entries */
	Tuplesortstate *sortstate;

	/* Counters */
	int64		numEntries;			/* total suffix entries generated */
	int64		numHeapTuples;		/* heap rows processed */

	/* Memory management */
	MemoryContext tmpCtx;			/* reset per heap tuple */
} SABuildState;


/* ----------------------------------------------------------------
 *				Function declarations -- sautils.c
 * ----------------------------------------------------------------
 */

/* Reloptions */
extern bytea *saoptions(Datum reloptions, bool validate);

/* State initialization */
extern void initSAState(SAState *state, Relation index);

/* Page initialization */
extern void SAInitPage(Page page, uint16 flags, Size pageSize);
extern void SAInitMetaPage(Page page, SAMetaPageData *meta, Size pageSize);

/* Entry comparison (byte-wise, for sorting and binary search) */
extern int	sa_compare_keys(const char *a, int alen,
							const char *b, int blen,
							int max_prefix_len);

/* Build phase name for progress reporting */
extern char *sabuildphasename(int64 phasenum);


/* ----------------------------------------------------------------
 *				Function declarations -- sabuild.c
 * ----------------------------------------------------------------
 */

extern IndexBuildResult *sabuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void sabuildempty(Relation index);


/* ----------------------------------------------------------------
 *				Function declarations -- sasearch.c
 * ----------------------------------------------------------------
 */

extern IndexScanDesc sabeginscan(Relation rel, int nkeys, int norderbys);
extern void saendscan(IndexScanDesc scan);
extern void sarescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern int64 sagetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * Binary search on the SA leaf section.
 * Finds the contiguous range [lo, hi) of global SA indices where
 * entries have sa_key starting with the given pattern.
 *
 * Returns true if a non-empty range was found.
 */
extern bool sa_binary_search(Relation index, SAMetaPageData *meta,
							 const char *pattern, int patternLen,
							 int64 *lo, int64 *hi);


/* ----------------------------------------------------------------
 *				AM handler
 * ----------------------------------------------------------------
 */

extern IndexAmRoutine *sahandler(PG_FUNCTION_ARGS);


#endif							/* SAINDEX_PRIVATE_H */

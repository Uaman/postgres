/*--------------------------------------------------------------------------
 * sabuild.c
 *	  Build routines for the suffix array index.
 *
 *	Build pipeline:
 *
 *	  1. GENERATE - Scan the heap.  For each row's text value, generate
 *	     one bytea sort datum per suffix position and feed it to a
 *	     tuplesort state.  Each datum encodes: zero-padded prefix key
 *	     (sort key), big-endian suffixlen (tiebreaker), offset, and heaptid.
 *
 *	  2. SORT - tuplesort_performsort() sorts all datums by bytea order
 *	     (byteacmp), which gives the same result as sa_compare_suffixes.
 *	     tuplesort spills to disk when maintenance_work_mem is exceeded,
 *	     so very large indexes no longer require all data to fit in RAM.
 *
 *	  3. WRITE ALL SECTIONS - Consume the sorted stream in a single pass.
 *	     For each datum, decode the payload and simultaneously write:
 *	       - SA leaf entry (with inline LCP) to the leaf section
 *	       - Suffix length to the sufflen section
 *	       - Heap TID to the TID section
 *	       - Bit to in-memory offset-zero and exact-suffix bitmaps
 *	     At stream end, flush the two in-memory bitmaps to bitmap pages.
 *
 *	  4. FINALIZE - Rewrite the meta page (block 0) with final section
 *	     descriptors.  WAL-log all pages.
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/backend/access/saindex/sabuild.c
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/saindex_private.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"


/* ----------------------------------------------------------------
 *				Sort datum encoding / decoding
 * ----------------------------------------------------------------
 *
 * Each suffix is encoded as a bytea datum for tuplesort.  The datum
 * layout is:
 *
 *   [0 .. maxPrefixLen-1]         key bytes, zero-padded  (primary sort key)
 *   [maxPrefixLen .. +1]          suffixlen  big-endian uint16  (tiebreaker)
 *   [maxPrefixLen+2 .. +5]        offset     big-endian int32
 *   [maxPrefixLen+6 .. +11]       heaptid    (6 bytes, native order)
 *
 * byteacmp on the full datum gives the same order as sa_compare_suffixes:
 *   - zero-padded key: identical to truncated suffix comparison
 *   - big-endian suffixlen: shorter suffix sorts first on key tie
 *   - offset and heaptid don't affect ordering (suffixlen breaks all ties
 *     for same-string values; different strings always differ in key bytes)
 */

/*
 * sa_encode_suffix
 *		Encode one suffix into a palloc'd bytea datum for tuplesort.
 */
static Datum
sa_encode_suffix(const char *suffix, int suffixlen, int32 offset,
				 ItemPointer heaptid, int32 maxPrefixLen, int32 datumSize)
{
	bytea	   *result;
	char	   *data;
	int			copyLen;
	uint16		sl_be;
	uint32		off_be;

	result = (bytea *) palloc(datumSize);
	SET_VARSIZE(result, datumSize);
	data = VARDATA(result);

	/* Zero-fill key area, then copy truncated suffix bytes */
	MemSet(data, 0, maxPrefixLen);
	copyLen = Min(suffixlen, maxPrefixLen);
	memcpy(data, suffix, copyLen);

	/* Big-endian uint16 suffixlen (tiebreaker: shorter first) */
	sl_be = pg_hton16((uint16) Min(suffixlen, PG_UINT16_MAX));
	memcpy(data + maxPrefixLen, &sl_be, sizeof(uint16));

	/* Big-endian int32 offset */
	off_be = pg_hton32((uint32) offset);
	memcpy(data + maxPrefixLen + sizeof(uint16), &off_be, sizeof(int32));

	/* heaptid (6 bytes) */
	memcpy(data + maxPrefixLen + sizeof(uint16) + sizeof(int32),
		   heaptid, sizeof(ItemPointerData));

	return PointerGetDatum(result);
}

/*
 * sa_decode_datum
 *		Decode a sort datum returned by tuplesort_getdatum.
 *
 * *keyOut points into the datum's memory and is valid only while the
 * datum is alive (i.e., until the next tuplesort_getdatum call with
 * copy=false).
 */
static void
sa_decode_datum(Datum d, int32 maxPrefixLen,
				char **keyOut, uint16 *suffixlenOut,
				int32 *offsetOut, ItemPointerData *heaptidOut)
{
	bytea	   *val = DatumGetByteaP(d);
	char	   *data = VARDATA(val);
	uint16		sl_be;
	uint32		off_be;

	*keyOut = data;

	memcpy(&sl_be, data + maxPrefixLen, sizeof(uint16));
	*suffixlenOut = pg_ntoh16(sl_be);

	memcpy(&off_be, data + maxPrefixLen + sizeof(uint16), sizeof(int32));
	*offsetOut = (int32) pg_ntoh32(off_be);

	memcpy(heaptidOut,
		   data + maxPrefixLen + sizeof(uint16) + sizeof(int32),
		   sizeof(ItemPointerData));
}


/* ----------------------------------------------------------------
 *				Heap scan callback
 * ----------------------------------------------------------------
 */

/*
 * sabuild_callback
 *		Process one heap tuple: extract the text value and feed one
 *		sort datum per suffix position to the tuplesort state.
 */
static void
sabuild_callback(Relation index, ItemPointer tid, Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	SABuildState   *bs = (SABuildState *) state;
	char		   *data;
	int				len;
	int				i;

	/* Skip NULLs and empty values — no suffixes to index */
	if (isnull[0])
		return;

	data = VARDATA_ANY(values[0]);
	len = VARSIZE_ANY_EXHDR(values[0]);

	if (len == 0)
		return;

	bs->numHeapTuples++;

	for (i = 0; i < len; i++)
	{
		Datum	d;

		d = sa_encode_suffix(data + i, len - i, i, tid,
							 bs->maxPrefixLen, bs->datumSize);
		tuplesort_putdatum(bs->sortstate, d, false);
		pfree(DatumGetPointer(d));
		bs->numEntries++;
	}
}


/* ----------------------------------------------------------------
 *				Unified section writer
 * ----------------------------------------------------------------
 */

/*
 * sa_flush_bitmaps
 *		Write two in-memory bitmaps to pages in the bitmap section.
 *
 * The offset-zero bitmap pages are written contiguously.  This matches
 * the layout expected by SABitmapBaseBlkno().
 */
static void
sa_flush_bitmaps(Relation index, int64 numEntries,
				 uint8 *offsetZeroBm,
				 BlockNumber *startBlkno, int32 *numPages,
				 uint16 *bitmapFlags)
{
	Size		bitmapBytes = (numEntries + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	BlockNumber firstBlkno = InvalidBlockNumber;
	int32		pagesWritten = 0;
	Size		bytesRemaining = bitmapBytes;
	Size		srcOffset = 0;

	while (bytesRemaining > 0)
	{
		Buffer	buf;
		Page	page;
		Size	chunkSize;

		buf = ReadBuffer(index, P_NEW);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		SAInitPage(page, SA_PAGE_BITMAP, BufferGetPageSize(buf));

		if (firstBlkno == InvalidBlockNumber)
			firstBlkno = BufferGetBlockNumber(buf);

		chunkSize = Min(bytesRemaining, (Size) SA_PAGE_DATA_SIZE);
		memcpy(SAPageGetBitmapData(page),
			   offsetZeroBm + srcOffset,
			   chunkSize);

		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);

		srcOffset += chunkSize;
		bytesRemaining -= chunkSize;
		pagesWritten++;
	}

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
	*bitmapFlags = SA_BITMAP_OFFSET_ZERO;
}

/*
 * sa_write_array_section
 *		Write a contiguous packed-array section from an in-memory buffer.
 *
 * itemSize is the per-entry byte size (2 for uint16, 6 for ItemPointerData).
 * itemsPerPage is computed by the caller from SA_PAGE_DATA_SIZE / itemSize.
 * pageFlags identifies the page type (SA_PAGE_SUFFLEN or SA_PAGE_TID).
 *
 * All pages are written contiguously, which is required for the O(1)
 * page-to-index mapping used during scans.
 */
static void
sa_write_array_section(Relation index,
					   const void *buf, int64 numEntries,
					   Size itemSize, int itemsPerPage,
					   uint16 pageFlags,
					   BlockNumber *startBlkno, int32 *numPages)
{
	const uint8 *src = (const uint8 *) buf;
	int64		remaining = numEntries;
	BlockNumber firstBlkno = InvalidBlockNumber;
	int32		pagesWritten = 0;

	while (remaining > 0)
	{
		int64	count = Min(remaining, (int64) itemsPerPage);
		Buffer	bufr;
		Page	page;

		bufr = ReadBuffer(index, P_NEW);
		LockBuffer(bufr, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(bufr);
		SAInitPage(page, pageFlags, BufferGetPageSize(bufr));

		if (firstBlkno == InvalidBlockNumber)
			firstBlkno = BufferGetBlockNumber(bufr);

		memcpy(SAPageGetData(page), src, count * itemSize);
		src += count * itemSize;
		remaining -= count;

		MarkBufferDirty(bufr);
		UnlockReleaseBuffer(bufr);
		pagesWritten++;
	}

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
}

/*
 * sa_write_all_sections
 *		Consume the sorted tuplesort stream and write all four index
 *		data sections: SA leaves, sufflen array, TID array, and bitmaps.
 *
 * The leaf section is written page-by-page during the streaming pass.
 * Auxiliary data (sufflen, TID, bitmaps) is accumulated in in-memory
 * arrays during the same pass and written as contiguous sections
 * afterwards.  This guarantees contiguous block layout for each section,
 * which is required for the O(1) index-to-page mapping in scan code.
 *
 * Memory cost of the aux arrays:
 *   sufflens : numEntries * 2 bytes
 *   tids     : numEntries * 6 bytes
 *   bitmaps  : 2 * ceil(numEntries / 8) bytes
 * For 320K entries this is about 2.6 MB — negligible.
 *
 * On entry, bs->sortstate must be ready to deliver tuples (i.e.,
 * tuplesort_performsort has been called).  On return, meta is updated
 * with the block/page counts for each section.
 */
static void
sa_write_all_sections(Relation index, SABuildState *bs, SAMetaPageData *meta)
{
	/* ---- Leaf section (streamed) ---- */
	Buffer		leafBuf = InvalidBuffer;
	Buffer		prevLeafBuf = InvalidBuffer;
	Page		leafPage = NULL;
	int			leafSlot = 0;
	BlockNumber leafFirst = InvalidBlockNumber;
	int32		leafPages = 0;

	/* ---- Aux data accumulated in memory ---- */
	uint16		   *sufflens;
	ItemPointerData *tids;
	Size			bitmapBytes;
	uint8		   *offsetZeroBm;

	/* ---- Previous-key buffer for LCP computation ---- */
	char	   *prevKeyBuf;
	int			prevEffLen = 0;

	/* ---- Loop state ---- */
	Datum		d;
	bool		isNull;
	int64		i = 0;

	if (bs->numEntries == 0)
		return;

	/* Allocate in-memory aux arrays */
	sufflens = palloc(bs->numEntries * sizeof(uint16));
	tids = palloc(bs->numEntries * sizeof(ItemPointerData));
	bitmapBytes = (bs->numEntries + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	offsetZeroBm = palloc0(bitmapBytes);
	prevKeyBuf = palloc(bs->maxPrefixLen);

	/* ---- Streaming pass: write leaf pages + fill aux arrays ---- */
	while (tuplesort_getdatum(bs->sortstate, true, false, &d, &isNull, NULL))
	{
		char		   *key;
		uint16			suffixlen;
		int32			offset;
		ItemPointerData heaptid;
		int				effLen;
		int				lcp;
		SAEntry		   *entry;

		Assert(!isNull);
		sa_decode_datum(d, bs->maxPrefixLen,
						&key, &suffixlen, &offset, &heaptid);

		effLen = Min((int) suffixlen, bs->maxPrefixLen);

		/* ---- Leaf page ---- */
		if (leafBuf == InvalidBuffer || leafSlot >= bs->entriesPerPage)
		{
			prevLeafBuf = leafBuf;

			leafBuf = ReadBuffer(index, P_NEW);
			LockBuffer(leafBuf, BUFFER_LOCK_EXCLUSIVE);
			leafPage = BufferGetPage(leafBuf);
			SAInitPage(leafPage, SA_PAGE_LEAF, BufferGetPageSize(leafBuf));

			if (leafFirst == InvalidBlockNumber)
				leafFirst = BufferGetBlockNumber(leafBuf);

			/* Set sa_next forward link on the completed previous page */
			if (prevLeafBuf != InvalidBuffer)
			{
				SAPageGetOpaque(BufferGetPage(prevLeafBuf))->sa_next =
					BufferGetBlockNumber(leafBuf);
				MarkBufferDirty(prevLeafBuf);
				UnlockReleaseBuffer(prevLeafBuf);
				prevLeafBuf = InvalidBuffer;
			}

			leafSlot = 0;
			leafPages++;
		}

		/* Compute LCP with previous sorted entry */
		lcp = (i > 0) ?
			sa_compute_lcp(prevKeyBuf, prevEffLen,
						   key, effLen,
						   bs->maxPrefixLen) : 0;

		/* Fill SAEntry on current leaf page */
		entry = SAPageGetEntry(leafPage, leafSlot, bs->entrySize);
		entry->sa_lcp = lcp;
		entry->sa_offset = offset;
		ItemPointerCopy(&heaptid, &entry->sa_heaptid);
		entry->sa_suffixlen = suffixlen;
		MemSet(entry->sa_key, 0, bs->maxPrefixLen);
		memcpy(entry->sa_key, key, effLen);
		leafSlot++;

		/* ---- Aux arrays ---- */
		sufflens[i] = suffixlen;
		ItemPointerCopy(&heaptid, &tids[i]);

		if (offset == 0)
		{
			int bytePos = i / BITS_PER_BYTE;
			int bitPos  = i % BITS_PER_BYTE;

			offsetZeroBm[bytePos] |= (1 << bitPos);
		}

		/*
		 * Save truncated key bytes for next LCP computation before
		 * calling tuplesort_getdatum again (copy=false means the current
		 * datum pointer may become invalid on the next call).
		 */
		memcpy(prevKeyBuf, key, effLen);
		prevEffLen = effLen;

		i++;
	}

	/* Flush the last leaf page */
	if (leafBuf != InvalidBuffer)
	{
		MarkBufferDirty(leafBuf);
		UnlockReleaseBuffer(leafBuf);
	}

	meta->sa_leaf_start = leafFirst;
	meta->sa_leaf_pages = leafPages;

	/* ---- Write sufflen section from memory (contiguous pages) ---- */
	sa_write_array_section(index,
						   sufflens, bs->numEntries,
						   sizeof(uint16), SA_SUFFLENS_PER_PAGE,
						   SA_PAGE_SUFFLEN,
						   &meta->sa_sufflen_start, &meta->sa_sufflen_pages);

	/* ---- Write TID section from memory (contiguous pages) ---- */
	sa_write_array_section(index,
						   tids, bs->numEntries,
						   sizeof(ItemPointerData), SA_TIDS_PER_PAGE,
						   SA_PAGE_TID,
						   &meta->sa_tid_start, &meta->sa_tid_pages);

	/* ---- Write bitmap section from memory ---- */
	sa_flush_bitmaps(index, bs->numEntries,
					 offsetZeroBm,
					 &meta->sa_bitmap_start, &meta->sa_bitmap_pages,
					 &meta->sa_bitmap_flags);

	pfree(sufflens);
	pfree(tids);
	pfree(offsetZeroBm);
	pfree(prevKeyBuf);
}


/* ----------------------------------------------------------------
 *				Meta page write helpers
 * ----------------------------------------------------------------
 */

/*
 * sa_write_meta_placeholder
 *		Create block 0 with an empty meta page.
 *
 * This must be the very first page allocated so that subsequent
 * ReadBuffer(index, P_NEW) calls yield blocks 1, 2, ... for the
 * data sections.
 */
static void
sa_write_meta_placeholder(Relation index, int maxPrefixLen)
{
	Buffer			buf;
	Page			page;
	SAMetaPageData	meta;

	buf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(buf) == SA_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	SAInitEmptyMeta(&meta, maxPrefixLen);
	SAInitMetaPage(page, &meta, BufferGetPageSize(buf));

	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

/*
 * sa_rewrite_meta
 *		Rewrite block 0 with the final, complete meta page data.
 *
 * Called after all data sections have been written and their block
 * ranges are known.
 */
static void
sa_rewrite_meta(Relation index, SAMetaPageData *meta)
{
	Buffer	buf;
	Page	page;

	buf = ReadBuffer(index, SA_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	SAInitMetaPage(page, meta, BufferGetPageSize(buf));

	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}


/* ----------------------------------------------------------------
 *				Top-level build entry points
 * ----------------------------------------------------------------
 */

/*
 * sabuild
 *		Main entry point for CREATE INDEX ... USING saindex.
 *
 * Builds a complete suffix array index by scanning the heap, sorting
 * all suffix entries via tuplesort (spills to disk if needed), and
 * writing the sorted data to contiguous index pages organized into
 * five sections.
 */
IndexBuildResult *
sabuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult   *result;
	SABuildState		bs;
	SAMetaPageData		meta;
	double				reltuples;
	TypeCacheEntry	   *typentry;

	/* Sanity check: index relation should be empty at this point */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* -------- Initialize build state -------- */

	bs.maxPrefixLen = SAGetMaxPrefixLen(index);
	bs.entrySize = SA_ENTRY_SIZE(bs.maxPrefixLen);
	bs.entriesPerPage = SAEntriesPerPage(bs.entrySize);
	bs.datumSize = VARHDRSZ + bs.maxPrefixLen + SA_SORT_DATUM_PAYLOAD_SIZE;
	bs.numEntries = 0;
	bs.numHeapTuples = 0;

	/*
	 * Initialize tuplesort on bytea datums.  byteacmp (via the bytea lt
	 * operator) gives the same sort order as sa_compare_suffixes because:
	 *   (a) the zero-padded key region sorts shorter suffixes first on tie,
	 *   (b) the big-endian suffixlen tiebreaker ensures shorter-first for
	 *       values that share a common prefix exceeding maxPrefixLen.
	 */
	typentry = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
	bs.sortstate = tuplesort_begin_datum(BYTEAOID,
										 typentry->lt_opr,
										 InvalidOid,	/* no collation */
										 false,			/* nulls not first */
										 maintenance_work_mem,
										 NULL,			/* no parallel sort */
										 TUPLESORT_NONE);

	/* -------- Phase 0: write placeholder meta page at block 0 -------- */

	sa_write_meta_placeholder(index, bs.maxPrefixLen);

	/* -------- Phase 1: scan heap, feed suffix datums to sortstate -------- */

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true,	/* allow_sync */
									   true,	/* progress */
									   sabuild_callback,
									   (void *) &bs, NULL);

	elog(NOTICE, "SA index \"%s\": %lld suffix entries from %.0f heap tuples",
		 RelationGetRelationName(index),
		 (long long) bs.numEntries, reltuples);

	/* -------- Phase 2: sort all entries -------- */

	tuplesort_performsort(bs.sortstate);

	/* -------- Phase 3: write data sections from sorted stream -------- */

	SAInitEmptyMeta(&meta, bs.maxPrefixLen);
	meta.sa_num_entries = bs.numEntries;
	meta.sa_num_heap_tuples = bs.numHeapTuples;

	sa_write_all_sections(index, &bs, &meta);

	/* -------- Phase 4: finalize meta page with real section data -------- */

	sa_rewrite_meta(index, &meta);

	/*
	 * WAL-log all pages at once.  During bulk build we skip per-page
	 * WAL logging and instead log the entire relation at the end.
	 * This follows the same pattern as GIN (see ginbuild).
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	/* -------- Cleanup -------- */

	tuplesort_end(bs.sortstate);

	/* -------- Return build statistics -------- */

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.numEntries;

	return result;
}

/*
 * sabuildempty
 *		Build an empty SA index on the INIT_FORKNUM.
 *
 * Called for unlogged tables.  Writes a single meta page with zero
 * entries and no data sections, so that the index is in a valid
 * (but empty) state after a crash recovery reset.
 */
void
sabuildempty(Relation index)
{
	Buffer			metaBuf;
	Page			metaPage;
	SAMetaPageData	meta;

	SAInitEmptyMeta(&meta, SAGetMaxPrefixLen(index));

	metaBuf = ReadBufferExtended(index, INIT_FORKNUM, P_NEW,
								 RBM_NORMAL, NULL);
	LockBuffer(metaBuf, BUFFER_LOCK_EXCLUSIVE);

	START_CRIT_SECTION();

	metaPage = BufferGetPage(metaBuf);
	SAInitMetaPage(metaPage, &meta, BufferGetPageSize(metaBuf));
	MarkBufferDirty(metaBuf);
	log_newpage_buffer(metaBuf, true);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(metaBuf);
}

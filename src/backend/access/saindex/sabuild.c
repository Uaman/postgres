/*--------------------------------------------------------------------------
 * sabuild.c
 *	  Build routines for the suffix array index.
 *
 *	Build pipeline:
 *
 *	  1. GENERATE - Scan the heap.  For each row's text value, generate
 *	     one SASortItem per suffix position.  All items are accumulated
 *	     in a dynamically-grown in-memory array.  The original text data
 *	     is copied into a dedicated memory context so suffix pointers
 *	     remain valid through sort and write.
 *
 *	  2. SORT - qsort the array by full suffix text (byte-wise, shorter
 *	     first on tie).  This establishes the canonical suffix array order.
 *
 *	  3. WRITE SA LEAVES - Iterate sorted items.  For each, construct a
 *	     fixed-size SAEntry (truncated key + LCP + metadata) and write it
 *	     to the current leaf page.  When a page fills, flush and allocate
 *	     the next.
 *
 *	  4. WRITE AUXILIARY ARRAYS - Second pass over sorted items writes:
 *	     - suffix-length array (packed uint16)
 *	     - TID array (packed ItemPointerData)
 *	     - offset-zero bitmap
 *	     - exact-suffix bitmap
 *
 *	  5. FINALIZE - Rewrite the meta page (block 0) with final section
 *	     descriptors.  WAL-log all pages.
 *
 *	Memory: all suffix text and the SASortItem array must fit in RAM.
 *	TODO: for very large tables, replace qsort with tuplesort-based
 *	external sort.
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


/* Initial capacity of the SASortItem array */
#define SA_INITIAL_ITEMS		1024


/* ----------------------------------------------------------------
 *				qsort comparison
 * ----------------------------------------------------------------
 */

static int
sa_sort_cmp(const void *a, const void *b)
{
	const SASortItem *ia = (const SASortItem *) a;
	const SASortItem *ib = (const SASortItem *) b;

	return sa_compare_suffixes(ia->suffix, ia->suffixlen,
							   ib->suffix, ib->suffixlen);
}


/* ----------------------------------------------------------------
 *				Heap scan callback
 * ----------------------------------------------------------------
 */

/*
 * sabuild_callback
 *		Process one heap tuple: extract the text value and generate
 *		one SASortItem for every suffix position.
 *
 * Text data is copied into buildstate->textCtx so that suffix pointers
 * remain valid after the heap scan moves on to subsequent tuples.
 */
static void
sabuild_callback(Relation index, ItemPointer tid, Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	SABuildState   *bs = (SABuildState *) state;
	MemoryContext	oldCtx;
	char		   *data;
	char		   *dataCopy;
	int				len;
	int				i;

	/* Skip NULLs — no suffixes to index */
	if (isnull[0])
		return;

	/* Extract raw bytes from the text/varchar/bytea datum */
	data = VARDATA_ANY(values[0]);
	len = VARSIZE_ANY_EXHDR(values[0]);

	/* Skip empty values — no suffixes */
	if (len == 0)
		return;

	bs->numHeapTuples++;

	/*
	 * Copy the text value into textCtx.  All suffixes of this value share
	 * this single allocation — they just point to different offsets within it.
	 */
	oldCtx = MemoryContextSwitchTo(bs->textCtx);
	dataCopy = palloc(len);
	memcpy(dataCopy, data, len);
	MemoryContextSwitchTo(oldCtx);

	/*
	 * Generate one SASortItem per suffix position [0, len).
	 * Grow the items array if needed.
	 */
	for (i = 0; i < len; i++)
	{
		SASortItem *item;

		/* Grow array with doubling strategy */
		if (bs->numEntries >= bs->maxEntries)
		{
			bs->maxEntries = Max(bs->maxEntries * 2, SA_INITIAL_ITEMS);
			bs->items = repalloc(bs->items,
								 bs->maxEntries * sizeof(SASortItem));
		}

		item = &bs->items[bs->numEntries];
		item->suffix = dataCopy + i;
		item->suffixlen = len - i;
		ItemPointerCopy(tid, &item->heaptid);
		item->offset = i;
		bs->numEntries++;
	}
}


/* ----------------------------------------------------------------
 *				Section writers
 * ----------------------------------------------------------------
 *
 * Each writer function iterates the sorted SASortItem array and writes
 * one section of the index.  Pages are allocated sequentially via
 * ReadBuffer(index, P_NEW), filled, marked dirty, and released.
 *
 * All writers return the starting BlockNumber and page count.
 */

/*
 * sa_write_leaves
 *		Write the SA leaf section: sorted SAEntry records with inline LCP.
 *
 * This is the core suffix array.  For each sorted item, we:
 *   - Compute LCP with the previous item
 *   - Construct a fixed-size SAEntry (truncated key, zero-padded)
 *   - Write it to the current leaf page
 *
 * On return, *startBlkno and *numPages describe the leaf section.
 */
static void
sa_write_leaves(Relation index, SABuildState *bs,
				BlockNumber *startBlkno, int32 *numPages)
{
	int32		maxPrefixLen = bs->maxPrefixLen;
	int32		entrySize = bs->entrySize;
	int32		perPage = bs->entriesPerPage;
	Buffer		curBuf = InvalidBuffer;
	Buffer		prevBuf = InvalidBuffer;
	Page		curPage = NULL;
	int			curSlot = 0;
	BlockNumber firstBlkno = InvalidBlockNumber;
	int32		pagesWritten = 0;
	int64		i;

	for (i = 0; i < bs->numEntries; i++)
	{
		SASortItem *item = &bs->items[i];
		SAEntry	   *entry;
		int			lcp;
		int			copyLen;

		/* Allocate a new page when the current one is full (or first time) */
		if (curBuf == InvalidBuffer || curSlot >= perPage)
		{
			/* Flush previous page before moving on */
			if (prevBuf != InvalidBuffer)
			{
				MarkBufferDirty(prevBuf);
				UnlockReleaseBuffer(prevBuf);
			}
			prevBuf = curBuf;

			curBuf = ReadBuffer(index, P_NEW);
			LockBuffer(curBuf, BUFFER_LOCK_EXCLUSIVE);
			curPage = BufferGetPage(curBuf);
			SAInitPage(curPage, SA_PAGE_LEAF, BufferGetPageSize(curBuf));

			if (firstBlkno == InvalidBlockNumber)
				firstBlkno = BufferGetBlockNumber(curBuf);

			/* Set previous page's sa_next forward link */
			if (prevBuf != InvalidBuffer)
			{
				SAPageGetOpaque(BufferGetPage(prevBuf))->sa_next =
					BufferGetBlockNumber(curBuf);
				MarkBufferDirty(prevBuf);
				UnlockReleaseBuffer(prevBuf);
				prevBuf = InvalidBuffer;
			}

			curSlot = 0;
			pagesWritten++;
		}

		/* Compute LCP with the previous entry in sorted order */
		lcp = 0;
		if (i > 0)
		{
			SASortItem *prev = &bs->items[i - 1];

			lcp = sa_compute_lcp(prev->suffix, prev->suffixlen,
								 item->suffix, item->suffixlen,
								 maxPrefixLen);
		}

		/* Fill the SAEntry on the current page */
		entry = SAPageGetEntry(curPage, curSlot, entrySize);

		entry->sa_lcp = lcp;
		entry->sa_offset = item->offset;
		ItemPointerCopy(&item->heaptid, &entry->sa_heaptid);
		entry->sa_suffixlen = (uint16) Min(item->suffixlen, PG_UINT16_MAX);

		/* Copy suffix text, truncated to maxPrefixLen, zero-padded */
		MemSet(entry->sa_key, 0, maxPrefixLen);
		copyLen = Min(item->suffixlen, maxPrefixLen);
		memcpy(entry->sa_key, item->suffix, copyLen);

		curSlot++;
	}

	/* Flush remaining pages */
	if (curBuf != InvalidBuffer)
	{
		MarkBufferDirty(curBuf);
		UnlockReleaseBuffer(curBuf);
	}
	if (prevBuf != InvalidBuffer)
	{
		MarkBufferDirty(prevBuf);
		UnlockReleaseBuffer(prevBuf);
	}

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
}

/*
 * sa_write_sufflens
 *		Write the suffix-length array section: packed uint16 values.
 */
static void
sa_write_sufflens(Relation index, SABuildState *bs,
				  BlockNumber *startBlkno, int32 *numPages)
{
	Buffer		curBuf = InvalidBuffer;
	Page		curPage = NULL;
	uint16	   *slots = NULL;
	int			curSlot = 0;
	BlockNumber firstBlkno = InvalidBlockNumber;
	int32		pagesWritten = 0;
	int64		i;

	for (i = 0; i < bs->numEntries; i++)
	{
		if (curBuf == InvalidBuffer || curSlot >= SA_SUFFLENS_PER_PAGE)
		{
			if (curBuf != InvalidBuffer)
			{
				MarkBufferDirty(curBuf);
				UnlockReleaseBuffer(curBuf);
			}

			curBuf = ReadBuffer(index, P_NEW);
			LockBuffer(curBuf, BUFFER_LOCK_EXCLUSIVE);
			curPage = BufferGetPage(curBuf);
			SAInitPage(curPage, SA_PAGE_SUFFLEN, BufferGetPageSize(curBuf));
			slots = SAPageGetSufflens(curPage);

			if (firstBlkno == InvalidBlockNumber)
				firstBlkno = BufferGetBlockNumber(curBuf);

			curSlot = 0;
			pagesWritten++;
		}

		slots[curSlot] = (uint16) Min(bs->items[i].suffixlen, PG_UINT16_MAX);
		curSlot++;
	}

	if (curBuf != InvalidBuffer)
	{
		MarkBufferDirty(curBuf);
		UnlockReleaseBuffer(curBuf);
	}

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
}

/*
 * sa_write_tids
 *		Write the TID array section: packed ItemPointerData values.
 */
static void
sa_write_tids(Relation index, SABuildState *bs,
			  BlockNumber *startBlkno, int32 *numPages)
{
	Buffer			curBuf = InvalidBuffer;
	Page			curPage = NULL;
	ItemPointerData *slots = NULL;
	int				curSlot = 0;
	BlockNumber		firstBlkno = InvalidBlockNumber;
	int32			pagesWritten = 0;
	int64			i;

	for (i = 0; i < bs->numEntries; i++)
	{
		if (curBuf == InvalidBuffer || curSlot >= SA_TIDS_PER_PAGE)
		{
			if (curBuf != InvalidBuffer)
			{
				MarkBufferDirty(curBuf);
				UnlockReleaseBuffer(curBuf);
			}

			curBuf = ReadBuffer(index, P_NEW);
			LockBuffer(curBuf, BUFFER_LOCK_EXCLUSIVE);
			curPage = BufferGetPage(curBuf);
			SAInitPage(curPage, SA_PAGE_TID, BufferGetPageSize(curBuf));
			slots = SAPageGetTids(curPage);

			if (firstBlkno == InvalidBlockNumber)
				firstBlkno = BufferGetBlockNumber(curBuf);

			curSlot = 0;
			pagesWritten++;
		}

		ItemPointerCopy(&bs->items[i].heaptid, &slots[curSlot]);
		curSlot++;
	}

	if (curBuf != InvalidBuffer)
	{
		MarkBufferDirty(curBuf);
		UnlockReleaseBuffer(curBuf);
	}

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
}

/*
 * sa_write_bitmaps
 *		Write the bitmap section: offset-zero and exact-suffix bitmaps.
 *
 * Both bitmaps are accumulated in memory first (they are small:
 * numEntries / 8 bytes each), then flushed to pages sequentially.
 * Bitmap 0 (offset-zero) pages come first, then bitmap 1 (exact-suffix).
 */
static void
sa_write_bitmaps(Relation index, SABuildState *bs,
				 BlockNumber *startBlkno, int32 *numPages,
				 uint16 *bitmapFlags)
{
	int64		numEntries = bs->numEntries;
	int32		maxPrefixLen = bs->maxPrefixLen;
	Size		bitmapBytes;
	uint8	   *offsetZeroBm;
	uint8	   *exactSuffixBm;
	int64		i;
	BlockNumber firstBlkno = InvalidBlockNumber;
	int32		pagesWritten = 0;

	if (numEntries == 0)
	{
		*startBlkno = InvalidBlockNumber;
		*numPages = 0;
		*bitmapFlags = 0;
		return;
	}

	/* Allocate in-memory bitmaps, zero-initialized (all bits clear) */
	bitmapBytes = (numEntries + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
	offsetZeroBm = palloc0(bitmapBytes);
	exactSuffixBm = palloc0(bitmapBytes);

	/* Populate bitmaps from sorted items */
	for (i = 0; i < numEntries; i++)
	{
		int		bytePos = i / BITS_PER_BYTE;
		int		bitPos = i % BITS_PER_BYTE;

		/* offset-zero: suffix starts at position 0 in the original value */
		if (bs->items[i].offset == 0)
			offsetZeroBm[bytePos] |= (1 << bitPos);

		/* exact-suffix: full suffix fits within stored prefix, no recheck */
		if (bs->items[i].suffixlen <= maxPrefixLen)
			exactSuffixBm[bytePos] |= (1 << bitPos);
	}

	/*
	 * Write bitmaps to pages.  Each bitmap occupies
	 * ceil(bitmapBytes / SA_PAGE_DATA_SIZE) pages.
	 * Order: all offset-zero pages, then all exact-suffix pages.
	 */
	{
		uint8	   *bitmaps[2] = {offsetZeroBm, exactSuffixBm};
		int			bm;

		for (bm = 0; bm < 2; bm++)
		{
			Size	bytesRemaining = bitmapBytes;
			Size	srcOffset = 0;

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
					   bitmaps[bm] + srcOffset,
					   chunkSize);

				MarkBufferDirty(buf);
				UnlockReleaseBuffer(buf);

				srcOffset += chunkSize;
				bytesRemaining -= chunkSize;
				pagesWritten++;
			}
		}
	}

	pfree(offsetZeroBm);
	pfree(exactSuffixBm);

	*startBlkno = firstBlkno;
	*numPages = pagesWritten;
	*bitmapFlags = SA_BITMAP_OFFSET_ZERO | SA_BITMAP_EXACT_SUFFIX;
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
 * all suffix entries in memory, and writing the sorted data to
 * contiguous index pages organized into five sections.
 */
IndexBuildResult *
sabuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult   *result;
	SABuildState		bs;
	SAMetaPageData		meta;
	double				reltuples;
	BlockNumber			blkno;
	int32				npages;

	/* Sanity check: index relation should be empty at this point */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* -------- Initialize build state -------- */

	bs.maxPrefixLen = SAGetMaxPrefixLen(index);
	bs.entrySize = SA_ENTRY_SIZE(bs.maxPrefixLen);
	bs.entriesPerPage = SAEntriesPerPage(bs.entrySize);

	bs.items = palloc(SA_INITIAL_ITEMS * sizeof(SASortItem));
	bs.numEntries = 0;
	bs.maxEntries = SA_INITIAL_ITEMS;
	bs.numHeapTuples = 0;

	bs.textCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "SA build text storage",
									   ALLOCSET_DEFAULT_SIZES);

	/* -------- Phase 0: write placeholder meta page at block 0 -------- */

	sa_write_meta_placeholder(index, bs.maxPrefixLen);

	/* -------- Phase 1: scan heap, generate suffix entries -------- */

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true,	/* allow_sync */
									   true,	/* progress */
									   sabuild_callback,
									   (void *) &bs, NULL);

	elog(NOTICE, "SA index \"%s\": %lld suffix entries from %.0f heap tuples",
		 RelationGetRelationName(index),
		 (long long) bs.numEntries, reltuples);

	/* -------- Phase 2: sort all entries by full suffix text -------- */

	if (bs.numEntries > 1)
		qsort(bs.items, bs.numEntries, sizeof(SASortItem), sa_sort_cmp);

	/* -------- Phase 3 & 4: write data sections -------- */

	SAInitEmptyMeta(&meta, bs.maxPrefixLen);
	meta.sa_num_entries = bs.numEntries;
	meta.sa_num_heap_tuples = bs.numHeapTuples;

	if (bs.numEntries > 0)
	{
		/* SA leaf pages (sorted entries with inline LCP) */
		sa_write_leaves(index, &bs, &blkno, &npages);
		meta.sa_leaf_start = blkno;
		meta.sa_leaf_pages = npages;

		/* Suffix-length array */
		sa_write_sufflens(index, &bs, &blkno, &npages);
		meta.sa_sufflen_start = blkno;
		meta.sa_sufflen_pages = npages;

		/* TID array */
		sa_write_tids(index, &bs, &blkno, &npages);
		meta.sa_tid_start = blkno;
		meta.sa_tid_pages = npages;

		/* Bitmaps (offset-zero + exact-suffix) */
		sa_write_bitmaps(index, &bs, &blkno, &npages,
						 &meta.sa_bitmap_flags);
		meta.sa_bitmap_start = blkno;
		meta.sa_bitmap_pages = npages;
	}

	/* -------- Phase 5: finalize meta page with real section data -------- */

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

	MemoryContextDelete(bs.textCtx);
	pfree(bs.items);

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

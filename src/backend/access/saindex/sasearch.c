/*--------------------------------------------------------------------------
 * sasearch.c
 *	  Search routines for the suffix array index.
 *
 *	Search pipeline (all strategies):
 *
 *	  1. BINARY SEARCH on SA leaf pages to find the contiguous range
 *	     [lo, hi) of global indices whose stored key prefix-matches
 *	     the query pattern.  O(log N) page reads.
 *
 *	  2. COLLECT TIDs according to the query strategy:
 *
 *	     CONTAINS (%x%):  every entry in [lo, hi) is a result.
 *	         Scan TID array pages sequentially, batch-add to TIDBitmap.
 *
 *	     PREFIX (x%):  filter entries where sa_offset == 0.
 *	         Scan offset-zero bitmap pages for [lo, hi),
 *	         then fetch TIDs for set bits from TID array pages.
 *
 *	     SUFFIX (%x):  filter entries where sa_suffixlen == patternLen.
 *	         Scan suffix-length array pages for [lo, hi),
 *	         then fetch TIDs for matching entries from TID array pages.
 *
 *	  3. RECHECK flag:  if patternLen > maxPrefixLen, the binary search
 *	     matched a truncated prefix, so the executor must recheck each
 *	     returned tuple against the original qual.  This is signaled
 *	     via the recheck parameter of tbm_add_tuples().
 *
 *	All auxiliary array scans read pages sequentially (one page at a
 *	time) and batch TID lookups by TID-array page to minimize I/O.
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/backend/access/saindex/sasearch.c
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/saindex_private.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


/* ----------------------------------------------------------------
 *				Binary search on SA leaf pages
 * ----------------------------------------------------------------
 */

/*
 * sa_read_entry_key
 *		Read one stored key from the SA leaf section by global index.
 *
 * Copies maxPrefixLen bytes of the key into caller-provided keyOut buffer.
 * The caller must ensure keyOut is at least maxPrefixLen bytes.
 */
static void
sa_read_entry_key(Relation index, SAMetaPageData *meta,
				  int64 pos, char *keyOut)
{
	BlockNumber blkno;
	int			slot;
	Buffer		buf;
	Page		page;
	SAEntry	   *entry;

	blkno = meta->sa_leaf_start + (BlockNumber) (pos / meta->sa_entries_per_page);
	slot = (int) (pos % meta->sa_entries_per_page);

	buf = ReadBuffer(index, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	entry = SAPageGetEntry(page, slot, meta->sa_entry_size);

	memcpy(keyOut, entry->sa_key, meta->sa_max_prefix_len);

	UnlockReleaseBuffer(buf);
}

/*
 * sa_binary_search
 *		Find the contiguous range [lo, hi) of SA entries whose stored
 *		key prefix-matches the given pattern.
 *
 * Uses two standard binary searches on the SA leaf section:
 *   lower_bound: first index where key >= pattern  (in prefix sense)
 *   upper_bound: first index where key >  pattern  (in prefix sense)
 *
 * Each probe reads one leaf page.  Total I/O: 2 * ceil(log2(N)) pages.
 *
 * Returns true if the range is non-empty.
 */
bool
sa_binary_search(Relation index, SAMetaPageData *meta,
				 const char *pattern, int patternLen,
				 int64 *lo_out, int64 *hi_out)
{
	int64		lo,
				hi,
				mid;
	int64		lower,
				upper;
	int			maxPrefixLen = meta->sa_max_prefix_len;
	char	   *keyBuf;
	int			cmp;

	if (meta->sa_num_entries == 0)
	{
		*lo_out = 0;
		*hi_out = 0;
		return false;
	}

	keyBuf = palloc(maxPrefixLen);

	/*
	 * Lower bound: find the first index where the stored key's prefix
	 * is >= the pattern.  Entries before this point have key < pattern.
	 */
	lo = 0;
	hi = meta->sa_num_entries;
	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		sa_read_entry_key(index, meta, mid, keyBuf);
		cmp = sa_compare_prefix(keyBuf, maxPrefixLen, pattern, patternLen);

		if (cmp < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	lower = lo;

	/*
	 * Upper bound: find the first index where the stored key's prefix
	 * is strictly > the pattern.  Entries from lower..upper-1 all have
	 * key prefix == pattern.
	 */
	lo = lower;
	hi = meta->sa_num_entries;
	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		sa_read_entry_key(index, meta, mid, keyBuf);
		cmp = sa_compare_prefix(keyBuf, maxPrefixLen, pattern, patternLen);

		if (cmp <= 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	upper = lo;

	pfree(keyBuf);

	*lo_out = lower;
	*hi_out = upper;

	return (lower < upper);
}


/* ----------------------------------------------------------------
 *				TID collection helpers
 * ----------------------------------------------------------------
 *
 * Each collector reads one or two auxiliary array sections to gather
 * matching TIDs into a TIDBitmap.  Pages are read sequentially and
 * TID-array reads are batched by page to minimize I/O.
 */

/*
 * sa_collect_all_tids
 *		CONTAINS strategy: every entry in [lo, hi) is a match.
 *		Scan TID array pages and batch-add all TIDs to the bitmap.
 */
static int64
sa_collect_all_tids(Relation index, SAMetaPageData *meta,
					int64 lo, int64 hi,
					TIDBitmap *tbm, bool recheck)
{
	int64		count = 0;
	BlockNumber first_tid_blk;
	BlockNumber last_tid_blk;
	BlockNumber blkno;

	first_tid_blk = SATidBlkno(meta, lo);
	last_tid_blk = SATidBlkno(meta, hi - 1);

	for (blkno = first_tid_blk; blkno <= last_tid_blk; blkno++)
	{
		Buffer			buf;
		Page			page;
		ItemPointerData *tids;
		int64			page_base;
		int				start_off;
		int				end_off;
		int				n;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(index, blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		tids = SAPageGetTids(page);

		/* Determine which slots on this page fall within [lo, hi) */
		page_base = (int64) (blkno - meta->sa_tid_start) * SA_TIDS_PER_PAGE;
		start_off = (int) (Max(lo, page_base) - page_base);
		end_off = (int) (Min(hi, page_base + SA_TIDS_PER_PAGE) - page_base);
		n = end_off - start_off;

		tbm_add_tuples(tbm, &tids[start_off], n, recheck);
		count += n;

		UnlockReleaseBuffer(buf);
	}

	return count;
}

/*
 * sa_fetch_tid
 *		Read one TID from the TID array by global index.
 *
 * Caller maintains a pinned TID page buffer for batching; this function
 * switches pages only when the target is on a different page.
 */
static inline void
sa_fetch_tid(Relation index, SAMetaPageData *meta, int64 pos,
			 Buffer *tid_buf, BlockNumber *cur_tid_blkno,
			 ItemPointerData *tidOut)
{
	BlockNumber blkno = SATidBlkno(meta, pos);
	int			off = SATidOffset(pos);

	if (blkno != *cur_tid_blkno)
	{
		if (*tid_buf != InvalidBuffer)
			UnlockReleaseBuffer(*tid_buf);

		*tid_buf = ReadBuffer(index, blkno);
		LockBuffer(*tid_buf, BUFFER_LOCK_SHARE);
		*cur_tid_blkno = blkno;
	}

	ItemPointerCopy(&SAPageGetTids(BufferGetPage(*tid_buf))[off], tidOut);
}

/*
 * sa_collect_bitmap_tids
 *		PREFIX strategy: scan the offset-zero bitmap for [lo, hi),
 *		collect TIDs only where the bit is set (sa_offset == 0).
 *
 * Reads bitmap pages sequentially; for each set bit, fetches the
 * corresponding TID from the TID array.  TID page reads are batched
 * because positions are in ascending order.
 */
static int64
sa_collect_bitmap_tids(Relation index, SAMetaPageData *meta,
					   int64 lo, int64 hi,
					   BlockNumber bm_base,
					   TIDBitmap *tbm, bool recheck)
{
	int64			count = 0;
	BlockNumber		bm_first;
	BlockNumber		bm_last;
	BlockNumber		bm_blkno;
	Buffer			tid_buf = InvalidBuffer;
	BlockNumber		cur_tid_blkno = InvalidBlockNumber;

	bm_first = SABitmapBlkno(bm_base, lo);
	bm_last = SABitmapBlkno(bm_base, hi - 1);

	for (bm_blkno = bm_first; bm_blkno <= bm_last; bm_blkno++)
	{
		Buffer		bm_buf;
		Page		bm_page;
		uint8	   *bm_data;
		int64		page_base;
		int64		scan_start;
		int64		scan_end;
		int64		pos;

		CHECK_FOR_INTERRUPTS();

		bm_buf = ReadBuffer(index, bm_blkno);
		LockBuffer(bm_buf, BUFFER_LOCK_SHARE);
		bm_page = BufferGetPage(bm_buf);
		bm_data = SAPageGetBitmapData(bm_page);

		page_base = (int64) (bm_blkno - bm_base) * SA_BITMAP_BITS_PER_PAGE;
		scan_start = Max(lo, page_base);
		scan_end = Min(hi, page_base + SA_BITMAP_BITS_PER_PAGE);

		for (pos = scan_start; pos < scan_end; pos++)
		{
			int		local_bit = (int) (pos - page_base);
			int		byteOff = local_bit / BITS_PER_BYTE;
			int		bitOff = local_bit % BITS_PER_BYTE;

			if (bm_data[byteOff] & (1 << bitOff))
			{
				ItemPointerData tid;

				sa_fetch_tid(index, meta, pos,
							 &tid_buf, &cur_tid_blkno, &tid);
				tbm_add_tuples(tbm, &tid, 1, recheck);
				count++;
			}
		}

		UnlockReleaseBuffer(bm_buf);
	}

	if (tid_buf != InvalidBuffer)
		UnlockReleaseBuffer(tid_buf);

	return count;
}

/*
 * sa_collect_sufflen_tids
 *		SUFFIX strategy: scan the suffix-length array for [lo, hi),
 *		collect TIDs only where sufflen == patternLen.
 *
 * A suffix of length exactly patternLen that starts with the pattern
 * IS the pattern — meaning the original value ends with the pattern.
 *
 * Reads sufflen pages sequentially; for each match, fetches the
 * corresponding TID from the TID array.
 */
static int64
sa_collect_sufflen_tids(Relation index, SAMetaPageData *meta,
						int64 lo, int64 hi, int patternLen,
						TIDBitmap *tbm, bool recheck)
{
	int64			count = 0;
	uint16			target = (uint16) Min(patternLen, PG_UINT16_MAX);
	BlockNumber		sl_first;
	BlockNumber		sl_last;
	BlockNumber		sl_blkno;
	Buffer			tid_buf = InvalidBuffer;
	BlockNumber		cur_tid_blkno = InvalidBlockNumber;

	sl_first = SASufflenBlkno(meta, lo);
	sl_last = SASufflenBlkno(meta, hi - 1);

	for (sl_blkno = sl_first; sl_blkno <= sl_last; sl_blkno++)
	{
		Buffer		sl_buf;
		Page		sl_page;
		uint16	   *sl_data;
		int64		page_base;
		int64		scan_start;
		int64		scan_end;
		int64		pos;

		CHECK_FOR_INTERRUPTS();

		sl_buf = ReadBuffer(index, sl_blkno);
		LockBuffer(sl_buf, BUFFER_LOCK_SHARE);
		sl_page = BufferGetPage(sl_buf);
		sl_data = SAPageGetSufflens(sl_page);

		page_base = (int64) (sl_blkno - meta->sa_sufflen_start) * SA_SUFFLENS_PER_PAGE;
		scan_start = Max(lo, page_base);
		scan_end = Min(hi, page_base + SA_SUFFLENS_PER_PAGE);

		for (pos = scan_start; pos < scan_end; pos++)
		{
			int		local_off = (int) (pos - page_base);

			if (sl_data[local_off] == target)
			{
				ItemPointerData tid;

				sa_fetch_tid(index, meta, pos,
							 &tid_buf, &cur_tid_blkno, &tid);
				tbm_add_tuples(tbm, &tid, 1, recheck);
				count++;
			}
		}

		UnlockReleaseBuffer(sl_buf);
	}

	if (tid_buf != InvalidBuffer)
		UnlockReleaseBuffer(tid_buf);

	return count;
}


/* ----------------------------------------------------------------
 *				Index scan API functions
 * ----------------------------------------------------------------
 */

/*
 * sabeginscan
 *		Allocate and initialize an IndexScanDesc for an SA index scan.
 *
 * Called once at the start of a scan.  The actual search parameters
 * are set later by sarescan().
 */
IndexScanDesc
sabeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	SAScanOpaque so;

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so = (SAScanOpaque) palloc0(sizeof(SAScanOpaqueData));

	/* Initialize the SA state from the meta page */
	initSAState(&so->sastate, rel);

	/* Read the full meta page for section descriptors */
	SAReadMeta(rel, &so->metaCopy);
	so->numEntries = so->metaCopy.sa_num_entries;

	/* Create a per-scan memory context for temporary allocations */
	so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
										"SA scan temp context",
										ALLOCSET_DEFAULT_SIZES);

	/* Query info will be set by sarescan() */
	so->strategy = InvalidStrategy;
	so->queryPattern = NULL;
	so->queryLen = 0;

	so->rangeValid = false;
	so->rangeLo = 0;
	so->rangeHi = 0;
	so->curPos = 0;
	so->scanDone = false;

	scan->opaque = so;

	return scan;
}

/*
 * sarescan
 *		Set or reset the search key for an SA index scan.
 *
 * Extracts the query pattern and strategy number from the ScanKey,
 * and copies the pattern into the scan's temp memory context.
 *
 * This is called once after sabeginscan() and may be called again
 * to restart the scan with different keys (e.g., after ReScan).
 */
void
sarescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	SAScanOpaque so = (SAScanOpaque) scan->opaque;
	MemoryContext oldCtx;

	/* Reset the temp context to free any previous query data */
	MemoryContextReset(so->tempCtx);

	/* Reset scan state */
	so->rangeValid = false;
	so->rangeLo = 0;
	so->rangeHi = 0;
	so->curPos = 0;
	so->scanDone = false;
	so->queryPattern = NULL;
	so->queryLen = 0;
	so->strategy = InvalidStrategy;

	if (scankey && nscankeys > 0)
	{
		Datum		arg;
		text	   *query;
		int			len;

		/*
		 * We expect exactly one scan key with a text argument.
		 * The strategy number tells us which query type.
		 */
		so->strategy = scankey[0].sk_strategy;

		arg = scankey[0].sk_argument;
		query = DatumGetTextPP(arg);
		len = VARSIZE_ANY_EXHDR(query);

		/* Copy the pattern text into the scan's temp context */
		oldCtx = MemoryContextSwitchTo(so->tempCtx);
		so->queryPattern = palloc(len + 1);
		memcpy(so->queryPattern, VARDATA_ANY(query), len);
		so->queryPattern[len] = '\0';
		so->queryLen = len;
		MemoryContextSwitchTo(oldCtx);
	}
}

/*
 * saendscan
 *		Clean up an SA index scan.
 *
 * Frees all memory associated with the scan opaque data.
 */
void
saendscan(IndexScanDesc scan)
{
	SAScanOpaque so = (SAScanOpaque) scan->opaque;

	if (so->tempCtx)
		MemoryContextDelete(so->tempCtx);

	pfree(so);
	scan->opaque = NULL;
}


/* ----------------------------------------------------------------
 *				Bitmap index scan entry point
 * ----------------------------------------------------------------
 */

/*
 * sagetbitmap
 *		Main entry point for bitmap index scans on an SA index.
 *
 * Pipeline:
 *   1. Binary search to find the range [lo, hi) of matching SA entries.
 *   2. Based on strategy, collect TIDs from auxiliary arrays into tbm.
 *   3. Set recheck if the pattern was truncated (queryLen > maxPrefixLen).
 *
 * Returns the number of TIDs added to the bitmap.
 */
int64
sagetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	SAScanOpaque so = (SAScanOpaque) scan->opaque;
	Relation	index = scan->indexRelation;
	SAMetaPageData *meta = &so->metaCopy;
	int64		lo,
				hi;
	bool		found;
	bool		recheck;
	int64		ntids = 0;

	/*
	 * If no scan key was set, return empty result.
	 */
	if (so->strategy == InvalidStrategy || so->queryPattern == NULL)
		return 0;

	/*
	 * Step 1: binary search on SA leaf pages.
	 *
	 * All three strategies use the same binary search: find the contiguous
	 * range of SA entries whose stored key prefix-matches the query pattern.
	 */
	found = sa_binary_search(index, meta,
							 so->queryPattern, so->queryLen,
							 &lo, &hi);

	if (!found)
		return 0;

	/*
	 * Determine recheck flag.
	 *
	 * If the query pattern is longer than the stored prefix, the binary
	 * search only matched on a truncated prefix, so the executor must
	 * verify each returned tuple against the full LIKE qual.
	 */
	recheck = (so->queryLen > meta->sa_max_prefix_len);

	/*
	 * Step 2: collect TIDs according to the query strategy.
	 */
	switch (so->strategy)
	{
		case SA_STRATEGY_CONTAINS:
			/*
			 * %x%: every suffix whose stored key starts with the pattern
			 * contains the pattern as a substring.  All entries in [lo, hi)
			 * are valid results.
			 */
			ntids = sa_collect_all_tids(index, meta, lo, hi, tbm, recheck);
			break;

		case SA_STRATEGY_PREFIX:
			/*
			 * x%: only entries where the suffix IS the full value (sa_offset
			 * == 0) are results.  The offset-zero bitmap identifies these
			 * entries efficiently.
			 */
			if (meta->sa_bitmap_flags & SA_BITMAP_OFFSET_ZERO)
			{
				BlockNumber bm_base;

				bm_base = SABitmapBaseBlkno(meta, SA_BITMAP_OFFSET_ZERO);
				ntids = sa_collect_bitmap_tids(index, meta, lo, hi,
											   bm_base, tbm, recheck);
			}
			else
			{
				/*
				 * Bitmap not available (shouldn't happen with a properly built
				 * index).  Fall back to returning all entries with recheck
				 * forced, so the executor will filter via the LIKE qual.
				 */
				ntids = sa_collect_all_tids(index, meta, lo, hi, tbm, true);
			}
			break;

		case SA_STRATEGY_SUFFIX:
			/*
			 * %x: only entries where the suffix length equals the pattern
			 * length are results.  If a suffix of length L starts with the
			 * pattern of length L, then the suffix IS the pattern, meaning
			 * the original value ends with the pattern.
			 *
			 * Use the suffix-length array for dense filtering.
			 */
			ntids = sa_collect_sufflen_tids(index, meta, lo, hi,
											so->queryLen, tbm, recheck);
			break;

		default:
			elog(ERROR, "unsupported SA index strategy number: %d",
				 so->strategy);
			break;
	}

	return ntids;
}

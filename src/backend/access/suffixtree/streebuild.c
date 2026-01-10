
#include "postgres.h"

#include "access/amapi.h"// API for Postgres index access methods.
#include "access/stree_private.h"// private declarations for Suffix Tree access method
#include "access/tableam.h"// for table_index_build_scan
#include "access/xloginsert.h"// for log_newpage_range
#include "nodes/execnodes.h"// definitions for executor state nodes
#include "miscadmin.h"// general postgres administration and initialization stuff
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/memutils.h"
#include "utils/rel.h"


static void streeBuildCallback(Relation index, ItemPointer tid, Datum *values,
                    bool *isnull, bool tupleIsAlive, void *state)
{
    STreeBuildState *buildState = (STreeBuildState *) state;
    MemoryContext oldCtx;

    elog(LOG, "streeBuildCallback: entering for tuple %u/%u", 
         ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));

    /* Build in temporary memory context, and reset it after each tuple insert */
    oldCtx = MemoryContextSwitchTo(buildState->tmpMemCtx);

    elog(LOG, "streeBuildCallback: switched memory context, calling streeinserttuple");

    /* Call insert once - no retry loop for now to debug */
    if (!streeinserttuple(index, buildState, tid, values, isnull))
    {
        elog(WARNING, "streeBuildCallback: streeinserttuple returned false");
    }

    elog(LOG, "streeBuildCallback: streeinserttuple returned");

    /* Update total tuple count */
    buildState->indexedTuples += 1;

    elog(LOG, "streeBuildCallback: switching back memory context");
    MemoryContextSwitchTo(oldCtx);
    
    elog(LOG, "streeBuildCallback: resetting tmpMemCtx");
    MemoryContextReset(buildState->tmpMemCtx);

    elog(LOG, "streeBuildCallback: completed for tuple %u/%u", 
         ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
}

/*
 * Build an Suffix Tree index during scan.
 */
IndexBuildResult *
streebuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	STreeBuildState buildState;
	Buffer		metabuffer,
				rootbuffer;

    //check if index is empty
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index (file) \"%s\" already contains data, cannot build, need clean one",
			 RelationGetRelationName(index));

	elog(LOG, "streebuild: starting, about to allocate metabuffer");

	/*
	 * Initialize the meta page and root pages
	 */
	metabuffer = STreeGetNewBuffer(index);
	elog(LOG, "streebuild: metabuffer allocated, blkno=%u", BufferGetBlockNumber(metabuffer));
	
	rootbuffer = STreeGetNewBuffer(index);
	elog(LOG, "streebuild: rootbuffer allocated, blkno=%u", BufferGetBlockNumber(rootbuffer));

	Assert(BufferGetBlockNumber(metabuffer) == STREE_METAPAGE_BLK);
	Assert(BufferGetBlockNumber(rootbuffer) == STREE_ROOT_BLK);

	START_CRIT_SECTION();

	STreeInitMetapage(BufferGetPage(metabuffer));
	MarkBufferDirty(metabuffer);
    
    Assert(BufferGetPageSize(rootbuffer) == BLCKSZ);
	STreeInitPage(BufferGetPage(rootbuffer), STREE_ROOT);
	MarkBufferDirty(rootbuffer);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuffer);
	UnlockReleaseBuffer(rootbuffer);

	elog(LOG, "streebuild: pages initialized");
	
	/*
	 * Now insert all the heap data into the index
	 */
	initSTreeState(&buildState.indexState, index);
	buildState.indexState.isBuild = true;
	buildState.indexedTuples = 0;

	buildState.tmpMemCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "STree build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	elog(LOG, "streebuild: about to call table_index_build_scan");

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   streeBuildCallback, &buildState,
									   NULL);

	elog(LOG, "streebuild: table_index_build_scan returned, reltuples=%f", reltuples);

	MemoryContextDelete(buildState.tmpMemCtx);

	//STreeUpdateMetaPage(index);

	/*
	 * If WAL-logging is
	 * required, write all pages to the WAL now.
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = buildState.indexedTuples;

	return result;
}


/*
 * Builds an empty SuffixTree index during the initialization
 */
void
streebuildempty(Relation index)
{
	BulkWriteState *writestate;
	BulkWriteBuffer bufwriter;

	writestate = smgr_bulk_start_rel(index, INIT_FORKNUM);

	// /* Construct metapage. */
	bufwriter = smgr_bulk_get_buf(writestate);
    STreeInitMetapage((Page) bufwriter);
	smgr_bulk_write(writestate, STREE_METAPAGE_BLK, bufwriter, true);

	/* Root page. */
	bufwriter = smgr_bulk_get_buf(writestate);
	STreeInitPage((Page) bufwriter, STREE_ROOT);
	smgr_bulk_write(writestate, STREE_ROOT_BLK, bufwriter, true);


	smgr_bulk_finish(writestate);
}

/*
 * Insert a single tuple into an existing suffix tree index.
 * This is called for each tuple during INSERT operations.
 */
bool
streeinsert(Relation index, Datum *values, bool *isnull,
            ItemPointer ht_ctid, Relation heapRel,
            IndexUniqueCheck checkUnique,
            bool indexUnchanged, IndexInfo *indexInfo)
{
    STreeBuildState buildState;
    MemoryContext oldCtx;
    MemoryContext tmpCtx;

    /* Create temporary memory context for this insert */
    tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
                                   "STree insert temporary context",
                                   ALLOCSET_DEFAULT_SIZES);
    oldCtx = MemoryContextSwitchTo(tmpCtx);

    /* Initialize state for insertion */
    initSTreeState(&buildState.indexState, index);
    buildState.indexState.isBuild = false;
    buildState.indexedTuples = 0;
    buildState.tmpMemCtx = tmpCtx;

    /*
     * Try to insert the tuple, retrying if we get a buffer-locking failure.
     */
    while (!streeinserttuple(index, &buildState, ht_ctid,
                             values, isnull))
    {
        MemoryContextReset(tmpCtx);
    }

    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(tmpCtx);

    /* Suffix tree index is never unique */
    return false;
}
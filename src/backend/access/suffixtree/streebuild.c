
#include "postgres.h"

#include "access/amapi.h"// API for Postgres index access methods.
#include "access/stree_private.h"// private declarations for Suffix Tree access method
#include "nodes/execnodes.h"// definitions for executor state nodes
#include "miscadmin.h"// general postgres administration and initialization stuff
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/memutils.h"


static void streeBuildCallback(Relation index, ItemPointer tid, Datum *values,
                    bool *isnull, bool tupleIsAlive, void *state)
{
    STreeBuildState *buildState = (STreeBuildState *) state;
    MemoryContext oldCtx;

    /* Build in temporary memory context, and reset it after each tuple insert */
    oldCtx = MemoryContextSwitchTo(buildState->tmpMemCtx);

    /*
     * Need to be ready to concurent insertion and getting a buffer-locking failure. 
     * Should be ready to retry.  We can flush
     * any temp data when retrying.
     */
    while (!streeinserttuple(index, &buildState->indexState, tid,
                        values, isnull))
    {
        MemoryContextReset(buildState->tmpMemCtx);
    }

    /* Update total tuple count */
    buildState->indexedTuples += 1;

    MemoryContextSwitchTo(oldCtx);
    MemoryContextReset(buildState->tmpMemCtx);
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

	/*
	 * Initialize the meta page and root pages
	 */
	metabuffer = STreeGetNewBuffer(index);
	rootbuffer = STreeGetNewBuffer(index);

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

	/*
	 * Now insert all the heap data into the index
	 */
	initSTreeState(&buildState.indexState, index);
	buildState.indexState.isBuild = true;
	buildState.indexedTuples = 0;

	buildState.tmpMemCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "STree build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   streeBuildCallback, &buildState,
									   NULL);

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
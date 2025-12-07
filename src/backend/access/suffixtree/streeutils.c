/**
 * 
 * streeutils.c
 *	  Suffix Tree index access method utility routines.
 * 
 * 
 * IDENTIFICATION
 *			src/backend/access/suffixtree/streeutils.c
 * 
 */


#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/stree_private.h"
#include "access/toast_compression.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_amop.h"
#include "commands/vacuum.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/catcache.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/**
 * Suffix Tree handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
streehandler(PG_FUNCTION_ARGS)
{

}


/**
 * Allocate a new buffer for the Suffix Tree index.
 * 
 * The returned buffer is already pinned and exclusive-locked.
 */
Buffer
STreeGetNewBuffer(Relation index)
{
    Buffer		buffer;

    /* Try to get a page from FSM (Free Space Map) */
    for (;;)
    {
        BlockNumber blkno = GetFreeIndexPage(index);

        if (blkno == InvalidBlockNumber)
            break;				/* nothing known to FSM */

        /*
         * The fixed pages shouldn't listed in FSM, we skip them.
         */
        if (StreeBlockIsFixed(blkno))
            continue;

        buffer = ReadBuffer(index, blkno);

        /*
         * ConditionalLockBuffer tries to acquire an exclusive lock on the buffer without blocking and waiting.
         * Avoiding possobolity that someone else already using this page; the buffer may be locked if so.
         */
        if (ConditionalLockBuffer(buffer))
        {
            Page		page = BufferGetPage(buffer);

            if (PageIsNew(page))
                return buffer;

            if (PageIsEmpty(page))
                return buffer;

            LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
        }

        /* Can't use it, so release buffer and try again */
        ReleaseBuffer(buffer);
    }
    
    return buffer;
}

void STreeInitMetapage(Page page) {
    STreeMetaPageData *metapage;

    /* Initialize the metapage */
    STreeInitPage(page, STREE_META);

    metapage = StreePageGetMetaStart(page);
    memset(metapage, 0, sizeof(STreeMetaPageData)); // zero out the metadata area (clean obtained from FSM or newly extended page)
    metapage->magicNumber = STREE_MAGIC_NUMBER;
    metapage->numberOfIndexedValues = 0;
    
    for (short i = 0; i < STREE_CACHED_PAGES; i++) {
        metapage->cachedPages[i].lastUsedPage = InvalidBlockNumber; // Initialize other metadata fields as needed
    }

    /*
     * Set pd_lower at the end of the metadata.  
     * In case xlog.c decides to compress the page, metadata will not be lost.
     */
    ((PageHeader) page)->pd_lower =
        ((char *) metapage + sizeof(STreeMetaPageData)) - (char *) page;
}

void STreeInitPage(Page page, uint16 f) {
    // Initialize the Suffix Tree page
    STreeNodePageOpaque opaque;

    PageInit(page, BLCKSZ, sizeof(STreeNodePageOpaqueData));
    opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);
    opaque->flags = f;
    opaque->streePageId = STREE_PAGE_ID;
    opaque->parentNode = InvalidBlockNumber;
    opaque->prevSiblingNode = InvalidBlockNumber;
    opaque->nextSiblingNode = InvalidBlockNumber;
    opaque->itemPointersStart = InvalidBlockNumber;
    opaque->firstNode = InvalidBlockNumber;
    opaque->suffixLink = InvalidBlockNumber;
}

void initSTreeState(STreeState *state, Relation index) {
    // Initialize the Suffix Tree state
    state->isBuild = false;
    state->index = index;
    state->redirectXid = GetTopTransactionIdIfAny();
}

/*
 * streeAllocateDataPage - Allocate a new data page for storing heap TIDs.
 *
 * This is called when a leaf edge has no data page yet.
 *
 * Parameters:
 *   index      - The relation
 *   leafBuffer - Buffer of the leaf node page (must be locked)
 *   edgeId     - The edge ID to update with the new data page
 *
 * Returns:
 *   Buffer of the new data page (locked), or InvalidBuffer on failure
 */
Buffer
streeAllocateDataPage(Relation index, Buffer leafBuffer, STreeEdgeIdData *edgeId)
{
    Buffer                  dataBuffer;
    Page                    dataPage;
    Page                    leafPage;
    STreeEdgeInsideData    *edgeData;
    STreeNodePageOpaque     opaque;
    STreeNodeTupleDataEntries *header;
    BlockNumber             dataBlkno;

    /* Allocate new page */
    dataBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(dataBuffer))
        return InvalidBuffer;

    dataBlkno = BufferGetBlockNumber(dataBuffer);
    LockBuffer(dataBuffer, BUFFER_LOCK_EXCLUSIVE);
    dataPage = BufferGetPage(dataBuffer);
    leafPage = BufferGetPage(leafBuffer);

    START_CRIT_SECTION();

    /* Initialize the data page */
    STreeInitPage(dataPage, STREE_DATA_NODE_PAGE);

    /* Set up opaque data */
    opaque = (STreeNodePageOpaque) PageGetSpecialPointer(dataPage);
    opaque->parentNode = BufferGetBlockNumber(leafBuffer);
    opaque->prevSiblingNode = InvalidBlockNumber;
    opaque->nextSiblingNode = InvalidBlockNumber;
    opaque->itemPointersStart = InvalidBlockNumber;
    opaque->firstNode = InvalidBlockNumber;
    opaque->suffixLink = InvalidBlockNumber;
    opaque->streePageId = STREE_PAGE_ID;
    opaque->flags = STREE_DATA_NODE_PAGE;

    /* Initialize the tuple entries header */
    header = (STreeNodeTupleDataEntries *) PageGetContents(dataPage);
    header->numberOfEntries = 0;

    /* Update the leaf edge to point to this data page */
    edgeData = streeGetEdgeData(leafPage, edgeId);
    edgeData->destinationNode = dataBlkno;

    MarkBufferDirty(dataBuffer);
    MarkBufferDirty(leafBuffer);

    END_CRIT_SECTION();

    return dataBuffer;
}

/*
 * streeAllocateOverflowDataPage - Allocate an overflow data page when current is full.
 *
 * Creates a chain of data pages for leaves with many TIDs.
 *
 * Parameters:
 *   index         - The relation
 *   leafBuffer    - Buffer of the leaf node page
 *   edgeId        - The edge ID
 *   currentBlkno  - Block number of the current (full) data page
 *
 * Returns:
 *   Buffer of the new overflow page (locked), or InvalidBuffer on failure
 */
Buffer
streeAllocateOverflowDataPage(Relation index, Buffer leafBuffer, 
                               STreeEdgeIdData *edgeId, BlockNumber currentBlkno)
{
    Buffer                  currentBuffer;
    Buffer                  newBuffer;
    Page                    currentPage;
    Page                    newPage;
    STreeNodePageOpaque     currentOpaque;
    STreeNodePageOpaque     newOpaque;
    STreeNodeTupleDataEntries *header;
    BlockNumber             newBlkno;

    /* Read current data page to update its next sibling link */
    currentBuffer = ReadBuffer(index, currentBlkno);
    LockBuffer(currentBuffer, BUFFER_LOCK_EXCLUSIVE);
    currentPage = BufferGetPage(currentBuffer);

    /* Allocate new overflow page */
    newBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(newBuffer))
    {
        UnlockReleaseBuffer(currentBuffer);
        return InvalidBuffer;
    }

    newBlkno = BufferGetBlockNumber(newBuffer);
    LockBuffer(newBuffer, BUFFER_LOCK_EXCLUSIVE);
    newPage = BufferGetPage(newBuffer);

    START_CRIT_SECTION();

    /* Initialize the new overflow page */
    STreeInitPage(newPage, STREE_DATA_NODE_PAGE);

    /* Set up opaque data for new page */
    newOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(newPage);
    newOpaque->parentNode = BufferGetBlockNumber(leafBuffer);
    newOpaque->prevSiblingNode = currentBlkno;
    newOpaque->nextSiblingNode = InvalidBlockNumber;
    newOpaque->itemPointersStart = InvalidBlockNumber;
    newOpaque->firstNode = InvalidBlockNumber;
    newOpaque->suffixLink = InvalidBlockNumber;
    newOpaque->streePageId = STREE_PAGE_ID;
    newOpaque->flags = STREE_DATA_NODE_PAGE;

    /* Initialize tuple entries header */
    header = (STreeNodeTupleDataEntries *) PageGetContents(newPage);
    header->numberOfEntries = 0;

    /* Update current page to point to new page */
    currentOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(currentPage);
    currentOpaque->nextSiblingNode = newBlkno;

    MarkBufferDirty(newBuffer);
    MarkBufferDirty(currentBuffer);

    END_CRIT_SECTION();

    UnlockReleaseBuffer(currentBuffer);

    return newBuffer;
}


/*
 * PageGetFreeSpaceEnd - Get the offset where free space ends (before edge data).
 *
 * This assumes edge data grows downward from the special pointer area.
 * You may need to track this in your page header or calculate it.
 */
// static Offset
// PageGetFreeSpaceEnd(Page page)
// {
//     STreeNodePageOpaque opaque;
    
//     opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);
    
//     /* 
//      * If you track the lowest edge data offset in opaque or header,
//      * return that. Otherwise, calculate based on existing edges.
//      */
//     return opaque->lowestEdgeDataOffset;
// }
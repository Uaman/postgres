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
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/catcache.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

/**
 * Suffix Tree handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
streehandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies = 1;        /* we support LIKE operator */
    amroutine->amsupport = 0;           /* no support functions needed */
    amroutine->amoptsprocnum = 0;
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = false;
    amroutine->amconsistentordering = false;
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = false;
    amroutine->amoptionalkey = false;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
    amroutine->amcanbuildparallel = false;
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
    amroutine->amkeytype = InvalidOid;

    /* Required callbacks */
    amroutine->ambuild = streebuild;
    amroutine->ambuildempty = streebuildempty;
    amroutine->aminsert = streeinsert;
    amroutine->aminsertcleanup = NULL;
    amroutine->ambulkdelete = NULL;         /* TODO: implement vacuum */
    amroutine->amvacuumcleanup = NULL;      /* TODO: implement vacuum cleanup */
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = streecostestimate;
    amroutine->amgettreeheight = NULL;
    amroutine->amoptions = NULL;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = streevalidate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = streebeginscan;
    amroutine->amrescan = streerescan;
    amroutine->amgettuple = streegettuple;
    amroutine->amgetbitmap = NULL;
    amroutine->amendscan = streeendscan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;

    PG_RETURN_POINTER(amroutine);
}


/**
 * Allocate a new buffer for the Suffix Tree index.
 * 
 * The returned buffer is already pinned and exclusive-locked.
 */
Buffer
STreeGetNewBuffer(Relation index)
{
    Buffer      buffer;
    int         fsmLoopCount = 0;
    const int   maxFsmLoops = 10;  /* Limit FSM attempts */

    /* Try to get a page from FSM (Free Space Map) */
    for (;;)
    {
        BlockNumber blkno = GetFreeIndexPage(index);

        fsmLoopCount++;
        if (fsmLoopCount > maxFsmLoops)
        {
            /* Too many FSM attempts - just extend */
            break;
        }

        if (blkno == InvalidBlockNumber)
            break;  /* nothing known to FSM */

        /*
         * Skip fixed pages (metapage, root) - they shouldn't be in FSM
         * but if they are, don't use them and break to extend.
         */
        if (STreeBlockIsFixed(blkno))
            break;

        buffer = ReadBuffer(index, blkno);

        /*
         * ConditionalLockBuffer tries to acquire exclusive lock without blocking.
         * If someone else is using this page, try another.
         */
        if (ConditionalLockBuffer(buffer))
        {
            Page page = BufferGetPage(buffer);

            if (PageIsNew(page) || PageIsEmpty(page))
            {
                /* Found a usable page */
                return buffer;
            }

            LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
        }

        /* Can't use it, release and try again */
        ReleaseBuffer(buffer);
    }

    /* No free page found - extend the relation */
    buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
                               EB_LOCK_FIRST);
    
    return buffer;
}

void STreeInitMetapage(Page page) {
    STreeMetaPageData *metapage;

    /* Initialize the metapage */
    STreeInitPage(page, STREE_META);

    metapage = STreePageGetMetaStart(page);
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
    STreeEdgeIdArrayHeader *edgeHeader;

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

    /*
     * For edge node pages (root and internal nodes), initialize the edge ID
     * array header and update pd_lower to account for it.
     */
    if (f == STREE_ROOT || f == STREE_EDGE_NODE_PAGE)
    {
        edgeHeader = STreePageGetEdgeIdHeader(page);
        edgeHeader->numberOfEdges = 0;
        
        /* Update pd_lower to point past the edge ID header */
        ((PageHeader) page)->pd_lower = 
            ((char *) edgeHeader + sizeof(STreeEdgeIdArrayHeader)) - (char *) page;
    }
}

void initSTreeState(STreeState *state, Relation index) {
    // Initialize the Suffix Tree state
    state->isBuild = false;
    state->index = index;
    state->redirectXid = GetTopTransactionIdIfAny();
}

/* ============================================================
 * Unified Data Page Operations
 * ============================================================
 * These functions handle data pages for storing heap TIDs.
 * They are generic and work for both:
 *   - Node data pages (via opaque->itemPointersStart)
 *   - Leaf edge data pages (via edge->destinationNode)
 */

/*
 * streeAllocateDataPage - Allocate a new data page for storing heap TIDs.
 *
 * This is a generic function that allocates a data page and links it
 * to the owner via the provided BlockNumber pointer.
 *
 * Parameters:
 *   index           - The relation
 *   ownerBuffer     - Buffer that will own this data page (for dirty marking)
 *   dataPageBlknoPtr - Where to store the new block number (updated on success)
 *
 * Returns:
 *   Buffer of the new data page (locked), or InvalidBuffer on failure
 */
Buffer
streeAllocateDataPage(Relation index, Buffer ownerBuffer, 
                      BlockNumber *dataPageBlknoPtr)
{
    Buffer                  dataBuffer;
    Page                    dataPage;
    STreeNodePageOpaque     opaque;
    STreeNodeTupleDataEntries *header;
    BlockNumber             dataBlkno;

    elog(LOG, "streeAllocateDataPage: entering");

    Assert(BufferIsValid(ownerBuffer));
    Assert(dataPageBlknoPtr != NULL);

    elog(LOG, "streeAllocateDataPage: calling STreeGetNewBuffer");
    /* Allocate new page - STreeGetNewBuffer returns a locked buffer */
    dataBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(dataBuffer))
    {
        elog(LOG, "streeAllocateDataPage: STreeGetNewBuffer returned invalid");
        return InvalidBuffer;
    }

    dataBlkno = BufferGetBlockNumber(dataBuffer);
    elog(LOG, "streeAllocateDataPage: got new buffer blkno=%u", dataBlkno);
    /* Buffer is already exclusively locked by STreeGetNewBuffer */
    dataPage = BufferGetPage(dataBuffer);

    START_CRIT_SECTION();

    /* Initialize the data page */
    STreeInitPage(dataPage, STREE_DATA_NODE_PAGE);

    /* Set up opaque data */
    opaque = (STreeNodePageOpaque) PageGetSpecialPointer(dataPage);
    opaque->parentNode = BufferGetBlockNumber(ownerBuffer);
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

    /* Update pd_lower to account for the tuple entries header */
    ((PageHeader) dataPage)->pd_lower = 
        ((char *) header + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE) - (char *) dataPage;

    /* Update the owner to point to this data page */
    *dataPageBlknoPtr = dataBlkno;

    MarkBufferDirty(dataBuffer);
    MarkBufferDirty(ownerBuffer);

    END_CRIT_SECTION();

    return dataBuffer;
}

/*
 * streeAllocateOverflowDataPage - Allocate overflow page when current is full.
 *
 * Creates a chain of data pages by linking the new page to the current one.
 *
 * Parameters:
 *   index         - The relation
 *   ownerBuffer   - Buffer that owns the data page chain (for parentNode)
 *   currentBlkno  - Block number of the current (full) data page
 *
 * Returns:
 *   Buffer of the new overflow page (locked), or InvalidBuffer on failure
 */
Buffer
streeAllocateOverflowDataPage(Relation index, Buffer ownerBuffer, 
                              BlockNumber currentBlkno)
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

    /* Allocate new overflow page - STreeGetNewBuffer returns a locked buffer */
    newBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(newBuffer))
    {
        UnlockReleaseBuffer(currentBuffer);
        return InvalidBuffer;
    }

    newBlkno = BufferGetBlockNumber(newBuffer);
    /* Buffer is already exclusively locked by STreeGetNewBuffer */
    newPage = BufferGetPage(newBuffer);

    START_CRIT_SECTION();

    /* Initialize the new overflow page */
    STreeInitPage(newPage, STREE_DATA_NODE_PAGE);

    /* Set up opaque data for new page */
    newOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(newPage);
    newOpaque->parentNode = BufferGetBlockNumber(ownerBuffer);
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

    /* Update pd_lower to account for the tuple entries header */
    ((PageHeader) newPage)->pd_lower = 
        ((char *) header + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE) - (char *) newPage;

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
 * streeAddHeapTidToDataPage - Add heap TID to a data page chain.
 *
 * This is the core function for adding TIDs. It handles:
 * - Allocating the first data page if needed
 * - Finding space in existing pages
 * - Allocating overflow pages when full
 *
 * Parameters:
 *   index            - The relation
 *   dataPageBlknoPtr - Pointer to BlockNumber storing first data page
 *                      (will be updated if new page allocated)
 *   ownerBuffer      - Buffer that owns this data page link (for dirty marking)
 *   tid              - The heap TID to store
 *
 * Returns:
 *   true on success, false on failure
 */
bool
streeAddHeapTidToDataPage(Relation index, BlockNumber *dataPageBlknoPtr,
                          Buffer ownerBuffer, ItemPointer tid)
{
    BlockNumber             dataPageBlkno;
    Buffer                  dataBuffer;
    Page                    dataPage;
    STreeNodeTupleDataEntries *header;
    STreeNodeTuple          tuples;
    Size                    spaceNeeded;
    Size                    freeSpace;

    elog(LOG, "streeAddHeapTidToDataPage: entering");

    Assert(BufferIsValid(ownerBuffer));
    Assert(dataPageBlknoPtr != NULL);
    Assert(tid != NULL);

    dataPageBlkno = *dataPageBlknoPtr;

    elog(LOG, "streeAddHeapTidToDataPage: dataPageBlkno=%u", dataPageBlkno);

    if (dataPageBlkno == InvalidBlockNumber)
    {
        elog(LOG, "streeAddHeapTidToDataPage: allocating new data page");
        /* Allocate new data page */
        dataBuffer = streeAllocateDataPage(index, ownerBuffer, dataPageBlknoPtr);
        if (!BufferIsValid(dataBuffer))
        {
            elog(LOG, "streeAddHeapTidToDataPage: allocation failed");
            return false;
        }
        elog(LOG, "streeAddHeapTidToDataPage: allocated data page %u", *dataPageBlknoPtr);
    }
    else
    {
        elog(LOG, "streeAddHeapTidToDataPage: reading existing data page");
        /* Read existing data page */
        dataBuffer = ReadBuffer(index, dataPageBlkno);
        LockBuffer(dataBuffer, BUFFER_LOCK_EXCLUSIVE);
    }

    dataPage = BufferGetPage(dataBuffer);
    header = (STreeNodeTupleDataEntries *) PageGetContents(dataPage);

    /* Calculate space needed */
    spaceNeeded = MAXALIGN(sizeof(IndexTupleData));
    freeSpace = PageGetFreeSpace(dataPage);

    if (freeSpace < spaceNeeded)
    {
        /* Data page is full - chain to new overflow page */
        BlockNumber currentBlkno = BufferGetBlockNumber(dataBuffer);
        UnlockReleaseBuffer(dataBuffer);
        
        dataBuffer = streeAllocateOverflowDataPage(index, ownerBuffer, currentBlkno);
        if (!BufferIsValid(dataBuffer))
            return false;
        
        dataPage = BufferGetPage(dataBuffer);
        header = (STreeNodeTupleDataEntries *) PageGetContents(dataPage);
    }

    START_CRIT_SECTION();

    /* Get pointer to tuple array and add new entry */
    tuples = (STreeNodeTuple) (((char *) header) + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE);
    
    /* Add new tuple at the end */
    STreeNodeTuple newTuple = &tuples[header->numberOfEntries];
    
    /* Initialize the index tuple with the heap TID */
    newTuple->t_tid = *tid;
    newTuple->t_info = 0;

    header->numberOfEntries++;

    /* Update pd_lower to account for the new tuple */
    ((PageHeader) dataPage)->pd_lower = 
        ((char *) header + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE + 
         (header->numberOfEntries * sizeof(STreeNodeTupleData))) - (char *) dataPage;

    MarkBufferDirty(dataBuffer);

    END_CRIT_SECTION();

    UnlockReleaseBuffer(dataBuffer);

    return true;
}

/*
 * streeGetDataPageTids - Iterate over all TIDs stored in a data page chain.
 *
 * This is used during index scans to retrieve matching heap TIDs.
 *
 * Parameters:
 *   index        - The relation
 *   dataBlkno    - Block number of the first data page
 *   callback     - Function to call for each TID
 *   callbackArg  - Argument passed to callback
 *
 * Returns:
 *   Number of TIDs processed
 */
int
streeGetDataPageTids(Relation index, BlockNumber dataBlkno,
                     void (*callback)(ItemPointer tid, void *arg), void *callbackArg)
{
    int                     totalTids = 0;
    BlockNumber             currentBlkno = dataBlkno;

    while (currentBlkno != InvalidBlockNumber)
    {
        Buffer                  buffer;
        Page                    page;
        STreeNodePageOpaque     opaque;
        STreeNodeTupleDataEntries *header;
        STreeNodeTuple          tuples;
        uint32                  i;

        buffer = ReadBuffer(index, currentBlkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        /* Validate page */
        opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);

        if (opaque->streePageId != STREE_PAGE_ID)
        {
            elog(WARNING, "streeGetDataPageTids: invalid streePageId=%u (expected %u)",
                 opaque->streePageId, STREE_PAGE_ID);
            UnlockReleaseBuffer(buffer);
            return totalTids;
        }

        header = (STreeNodeTupleDataEntries *) PageGetContents(page);
        tuples = (STreeNodeTuple) (((char *) header) + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE);

        /* Process each TID */
        for (i = 0; i < header->numberOfEntries; i++)
        {
            if (callback)
                callback(&tuples[i].t_tid, callbackArg);
            totalTids++;
        }

        /* Move to next page in chain */
        currentBlkno = opaque->nextSiblingNode;

        UnlockReleaseBuffer(buffer);
    }

    return totalTids;
}

/*
 * streevalidate - Validate an operator class for suffix tree
 */
bool
streevalidate(Oid opclassoid)
{
    /* For now, accept any text operator class */
    /* TODO: Add proper validation for LIKE operator support */
    return true;
}

/*
 * streecostestimate - Estimate the cost of an index scan
 */
void
streecostestimate(PlannerInfo *root, IndexPath *path,
                  double loop_count, Cost *indexStartupCost,
                  Cost *indexTotalCost, Selectivity *indexSelectivity,
                  double *indexCorrelation, double *indexPages)
{
    GenericCosts costs = {0};
    
    /* Use generic cost estimation as a starting point */
    genericcostestimate(root, path, loop_count, &costs);
    
    /*
     * Suffix tree scans are typically O(m) where m is pattern length,
     * plus O(k) where k is the number of matches. This is generally
     * faster than sequential scan for substring searches.
     */
    *indexStartupCost = costs.indexStartupCost;
    *indexTotalCost = costs.indexTotalCost;
    *indexSelectivity = costs.indexSelectivity;
    *indexCorrelation = costs.indexCorrelation;
    *indexPages = costs.numIndexPages;
}
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


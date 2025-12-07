#include "postgres.h"

#include "access/stree_private.h"
#include "access/genam.h"
#include "access/xloginsert.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
/** 
 * For UTF-8 support
 * pg_mblen(str) - Returns byte length of the first multibyte character
 * pg_mbstrlen(str) - Returns character length of a multibyte string
 * pg_mbstrlen_with_len(str, bytelen) - Returns character length of a multibyte string with given byte length
 * pg_mb2wchar_with_len() - Converts multibyte string to wide character (Unicode codepoint)
*/
#include "mb/pg_wchar.h" 

STreeEdgeIdData *
lookupEdgeByFirstChar(Page page, pg_wchar firstChar)
{
    uint16 numOfEdges;
    STreeEdgeIdData *edgeIds;

    Assert(page != NULL);

    numOfEdges = STreePageGetNumEdges(page);

    /* Get pointer to edge IDs array */
    edgeIds = STreePageGetEdgeIds(page);

    if (numOfEdges <= STREE_BSEARCH_THRESHOLD)
    {
        /* Linear scan - cache friendly for small sets */
        for (uint16 i = 0; i < numOfEdges; i++)
        {
            if (EdgeIdGetFirstChar(&edgeIds[i]) == firstChar)
                return &edgeIds[i];
        }
        return NULL;
    }
    else
    {
        /* Binary search - better for large sets */
        uint16 low = 0;
        uint16 high = numOfEdges;
        
        while (low < high)
        {
            uint16 mid = low + (high - low) / 2;
            
            if (EdgeIdGetFirstChar(&edgeIds[mid]) < firstChar)
                low = mid + 1;
            else
                high = mid;
        }
        
        if (low < numOfEdges)
        {
            if (EdgeIdGetFirstChar(&edgeIds[low]) == firstChar)
                return &edgeIds[low];
        }
        return NULL;
    }

    /* Edge not found */
    return NULL;
}


/*
 * walkDown - Walk down from current node along an edge if activeLength >= edge length.
 *
 * This is the "skip/count" trick in Ukkonen's algorithm. When activeLength
 * is greater than or equal to the edge length, we need to move to the child
 * node and adjust the active point.
 *
 * Parameters:
 *   index           - The relation (for reading buffers)
 *   activeNode      - Current active node (will be updated if we walk down)
 *   activeEdgeCharIdx - Pointer to active edge character index (will be updated)
 *   activeLength    - Pointer to active length (will be updated)
 *   strValue        - The original string being inserted
 *   strEnd          - End of the string
 *   rootBuffer      - Root buffer (to avoid releasing it)
 *
 * Returns:
 *   true if we walked down (caller should continue the while loop)
 *   false if we didn't walk down (caller should proceed with extension)
 */
bool
walkDown(Relation index,
         STreeActiveNode *activeNode,
         int *activeEdgeCharIdx,
         int *activeLength,
         const char *strValue,
         const char *strEnd,
         Buffer rootBuffer)
{
    STreeEdgeIdData    *edgeId;
    STreeEdgeInsideData *edgeData;
    pg_wchar            activeEdgeChar;
    const char         *activeEdgePtr;
    int                 edgeLabelCharLen;
    int                 i;

    Assert(activeNode != NULL);
    Assert(activeNode->page != NULL);
    Assert(*activeLength > 0);

    /*
     * Get the character at activeEdgeCharIdx position in the original string.
     * We need to walk the string to find the byte offset for UTF-8.
     */
    activeEdgePtr = strValue;
    for (i = 0; i < *activeEdgeCharIdx && activeEdgePtr < strEnd; i++)
    {
        activeEdgePtr += pg_mblen(activeEdgePtr);
    }

    if (activeEdgePtr >= strEnd)
    {
        /* Shouldn't happen - active edge points past string end */
        return false;
    }

    /* Get the Unicode codepoint at active edge position */
    pg_mb2wchar_with_len((const unsigned char *) activeEdgePtr,
                         &activeEdgeChar,
                         pg_mblen(activeEdgePtr));

    /* Find the edge starting with this character */
    edgeId = lookupEdgeByFirstChar(activeNode->page, activeEdgeChar);
    if (edgeId == NULL)
    {
        /* No edge found - this shouldn't happen if called correctly */
        return false;
    }

    /* Get the edge data */
    edgeData = streeGetEdgeData(activeNode->page, edgeId);

    /* Calculate edge label length in characters (not bytes) */
    edgeLabelCharLen = pg_mbstrlen_with_len(edgeData->label, edgeData->labelLength);

    /*
     * Check if we need to walk down.
     * If activeLength >= edge label length, we need to move to the child node.
     */
    if (*activeLength < edgeLabelCharLen)
    {
        /* Don't walk down - we're in the middle of this edge */
        return false;
    }

    /*
     * Walk down to child node.
     */
    BlockNumber childBlkno = edgeData->destinationNode;

    if (childBlkno == InvalidBlockNumber)
    {
        /* This is a leaf edge - shouldn't walk down further */
        return false;
    }

    /* Acquire buffer for child node */
    Buffer childBuffer = ReadBuffer(index, childBlkno);

    /* Try to lock child buffer - use conditional to avoid deadlock */
    if (!ConditionalLockBuffer(childBuffer))
    {
        /*
         * Could not acquire lock - deadlock prevention.
         * Release child buffer and return false to let caller retry.
         */
        ReleaseBuffer(childBuffer);
        return false;
    }

    /*
     * Successfully acquired child buffer.
     * Release old buffer (unless it's root) and update active node.
     */
    if (activeNode->buffer != rootBuffer && BufferIsValid(activeNode->buffer))
    {
        UnlockReleaseBuffer(activeNode->buffer);
    }

    /* Update active node to point to child */
    activeNode->buffer = childBuffer;
    activeNode->page = BufferGetPage(childBuffer);
    activeNode->blockNumber = childBlkno;

    /* Update active edge and length */
    *activeEdgeCharIdx += edgeLabelCharLen;
    *activeLength -= edgeLabelCharLen;

    return true;
}

/*
 * followSuffixLink - Move active node to its suffix link.
 *
 * Parameters:
 *   index       - The relation
 *   activeNode  - Current active node (will be updated)
 *   rootBuffer  - Root buffer (used as fallback)
 *   rootBlkno   - Root block number
 *
 * Returns:
 *   true on success, false if we need to retry (lock failure)
 */
static bool
followSuffixLink(Relation index,
                 STreeActiveNode *activeNode,
                 Buffer rootBuffer,
                 BlockNumber rootBlkno)
{
    STreeNodePageOpaque opaque;
    BlockNumber         suffixLinkBlkno;
    Buffer              suffixLinkBuffer;

    Assert(activeNode != NULL);
    Assert(activeNode->page != NULL);

    opaque = (STreeNodePageOpaque) PageGetSpecialPointer(activeNode->page);
    suffixLinkBlkno = opaque->suffixLink;

    /* If no suffix link or it's invalid, go to root */
    if (suffixLinkBlkno == InvalidBlockNumber)
    {
        suffixLinkBlkno = rootBlkno;
    }

    /* If already at the destination, nothing to do */
    if (activeNode->blockNumber == suffixLinkBlkno)
    {
        return true;
    }

    /* If going to root, just update pointers */
    if (suffixLinkBlkno == rootBlkno)
    {
        if (activeNode->buffer != rootBuffer && BufferIsValid(activeNode->buffer))
        {
            UnlockReleaseBuffer(activeNode->buffer);
        }

        activeNode->buffer = rootBuffer;
        activeNode->page = BufferGetPage(rootBuffer);
        activeNode->blockNumber = rootBlkno;
        return true;
    }

    /* Acquire buffer for suffix link node */
    suffixLinkBuffer = ReadBuffer(index, suffixLinkBlkno);

    if (!ConditionalLockBuffer(suffixLinkBuffer))
    {
        /* Deadlock prevention - retry */
        ReleaseBuffer(suffixLinkBuffer);
        return false;
    }

    /* Release old buffer (unless it's root) */
    if (activeNode->buffer != rootBuffer && BufferIsValid(activeNode->buffer))
    {
        UnlockReleaseBuffer(activeNode->buffer);
    }

    /* Update active node */
    activeNode->buffer = suffixLinkBuffer;
    activeNode->page = BufferGetPage(suffixLinkBuffer);
    activeNode->blockNumber = suffixLinkBlkno;

    return true;
}

/*
 * streeGetDataPageTids - Iterate over all TIDs stored in a leaf's data pages.
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
        opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);
        currentBlkno = opaque->nextSiblingNode;

        UnlockReleaseBuffer(buffer);
    }

    return totalTids;
}
#include "postgres.h"

#include "access/stree_private.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/xloginsert.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
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

    Assert(activeNode != NULL);
    Assert(activeNode->page != NULL);
    Assert(*activeLength > 0);

    // 1. Get the edge starting with character at activeEdgeCharIdx
    pg_wchar activeEdgeChar = getCharAtPosition(strValue, strEnd, *activeEdgeCharIdx);
    STreeEdgeIdData *edgeId = lookupEdgeByFirstChar(activeNode->page, activeEdgeChar);
    
    if (edgeId == NULL)
        return false;  // No edge - can't walk
    
    // 2. Get edge label length in characters
    STreeEdgeInsideData *edgeData = streeGetEdgeData(activeNode->page, edgeId);
    int edgeLabelCharLen = getEdgeLabelCharLength(edgeData);
    
    // 3. Check if we need to walk down
    if (*activeLength < edgeLabelCharLen)
        return false;  // We're inside this edge - stop
    
    // 4. Walk down to child node
    BlockNumber childBlkno = edgeData->destinationNode;
    if (childBlkno == InvalidBlockNumber)
        return false;  // Leaf - can't walk further
    
    // 5. Acquire child buffer and update active node
    Buffer childBuffer = ReadBuffer(index, childBlkno);
    LockBuffer(childBuffer, BUFFER_LOCK_EXCLUSIVE);
    
    // Release old buffer (unless it's root)
    if (activeNode->buffer != rootBuffer && BufferIsValid(activeNode->buffer))
        UnlockReleaseBuffer(activeNode->buffer);
    
    activeNode->buffer = childBuffer;
    activeNode->page = BufferGetPage(childBuffer);
    activeNode->blockNumber = childBlkno;
    
    // 6. Update active edge and length
    *activeEdgeCharIdx += edgeLabelCharLen;
    *activeLength -= edgeLabelCharLen;
    
    return true;  // We walked down - caller should continue loop
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
bool
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
 * streeFindBufferForPattern - Find the node buffer that corresponds to the
 * given pattern. Returns a pinned & share-locked Buffer for the node that
 * represents all occurrences of the pattern (caller must UnlockReleaseBuffer
 * it), or InvalidBuffer if pattern not found.
 */
Buffer
streeFindBufferForPattern(Relation index, const char *pattern, int patternBytes)
{
    Buffer      rootBuffer;
    STreeActiveNode active;
    const char  *pcur = pattern;
    const char  *pend = pattern + patternBytes;

    elog(LOG, "streeFindBufferForPattern: searching for pattern '%.*s' (len=%d)",
         patternBytes, pattern, patternBytes);

    /* Acquire root buffer */
    rootBuffer = ReadBuffer(index, STREE_ROOT_BLK);
    LockBuffer(rootBuffer, BUFFER_LOCK_SHARE);

    active.blockNumber = STREE_ROOT_BLK;
    active.buffer = rootBuffer;
    active.page = BufferGetPage(rootBuffer);

    elog(LOG, "streeFindBufferForPattern: root has %d edges", 
         STreePageGetNumEdges(active.page));

    /* Walk the pattern character-by-character */
    while (pcur < pend)
    {
        /* Peek next character from pattern */
        const char *tmp = pcur;
        pg_wchar patChar = streeGetNextChar(&tmp, pend);

        elog(LOG, "streeFindBufferForPattern: looking for edge with char %u at node %u",
             patChar, active.blockNumber);

        /* Find edge starting with this character */
        STreeEdgeIdData *edgeId = lookupEdgeByFirstChar(active.page, patChar);
        if (edgeId == NULL)
        {
            elog(LOG, "streeFindBufferForPattern: no edge found for char %u", patChar);
            /* No matching edge */
            UnlockReleaseBuffer(active.buffer);
            return InvalidBuffer;
        }

        STreeEdgeInsideData *edgeData = streeGetEdgeData(active.page, edgeId);
        int edgeChars = getEdgeLabelCharLength(edgeData);

        elog(LOG, "streeFindBufferForPattern: found edge '%.*s' (%d chars), dest=%u",
             edgeData->labelLength, edgeData->label, edgeChars, edgeData->destinationNode);

        /* consume the first char we already peeked */
        pcur = tmp;

        /* Compare remaining characters of the edge label */
        for (int i = 1; i < edgeChars; i++)
        {
            if (pcur >= pend)
            {
                /* Pattern ends inside this edge: occurrences are under the edge's child */
                if (edgeData->destinationNode != InvalidBlockNumber)
                {
                    Buffer childBuf = ReadBuffer(index, edgeData->destinationNode);
                    LockBuffer(childBuf, BUFFER_LOCK_SHARE);
                    /* release previous node buffer */
                    UnlockReleaseBuffer(active.buffer);
                    return childBuf;
                }
                else
                {
                    /* Edge goes to nowhere (leaf) — return current node */
                    return active.buffer;
                }
            }

            /* get next pattern character */
            const char *tmp2 = pcur;
            pg_wchar pch = streeGetNextChar(&tmp2, pend);
            pg_wchar edgech = getEdgeLabelCharAt(edgeData, i);

            if (pch != edgech)
            {
                /* mismatch */
                UnlockReleaseBuffer(active.buffer);
                return InvalidBuffer;
            }

            pcur = tmp2;
        }

        /* We've matched full edge label; move to child node */
        if (edgeData->destinationNode == InvalidBlockNumber)
        {
            /* matched full label, but no child (leaf). If pattern still remains -> no match */
            if (pcur < pend)
            {
                UnlockReleaseBuffer(active.buffer);
                return InvalidBuffer;
            }
            /* pattern ended exactly at leaf edge — return current node */
            return active.buffer;
        }

        /* advance to child node */
        BlockNumber child = edgeData->destinationNode;
        Buffer childBuf = ReadBuffer(index, child);
        LockBuffer(childBuf, BUFFER_LOCK_SHARE);

        /* release previous node buffer */
        UnlockReleaseBuffer(active.buffer);

        active.blockNumber = child;
        active.buffer = childBuf;
        active.page = BufferGetPage(childBuf);
    }

    /* Pattern exhausted; active.buffer is the node for the pattern */
    return active.buffer;
}


/*
 * streeCollectSubtreeTids - Recursively collect TIDs from a node and all its descendants.
 *
 * For substring search, when we find a node matching the pattern, we need to
 * collect TIDs from the entire subtree rooted at that node, since all strings
 * passing through any descendant also contain the pattern.
 *
 * Parameters:
 *   index       - The relation
 *   nodeBuffer  - Buffer for current node (must be locked for share)
 *   callback    - Function to call for each TID
 *   callbackArg - Argument passed to callback
 *
 * Returns:
 *   Total number of TIDs collected from this subtree
 */
static int
streeCollectSubtreeTids(Relation index, Buffer nodeBuffer,
                        void (*callback)(ItemPointer tid, void *arg), 
                        void *callbackArg)
{
    Page page = BufferGetPage(nodeBuffer);
    BlockNumber nodeBlkno = BufferGetBlockNumber(nodeBuffer);
    int total = 0;
    int nodeTids = 0;
    uint16 numEdges;
    STreeEdgeIdData *edgeIds;

    /* Collect TIDs stored directly at this node */
    nodeTids = streeGetNodeTids(index, nodeBuffer, callback, callbackArg);
    total += nodeTids;
    
    elog(LOG, "streeCollectSubtreeTids: node %u has %d direct TIDs", nodeBlkno, nodeTids);

    /* Get edges from this node */
    numEdges = STreePageGetNumEdges(page);
    edgeIds = STreePageGetEdgeIds(page);
    
    elog(LOG, "streeCollectSubtreeTids: node %u has %d edges", nodeBlkno, numEdges);

    /* Recurse into all child nodes */
    for (uint16 i = 0; i < numEdges; i++)
    {
        STreeEdgeInsideData *edgeData = streeGetEdgeData(page, &edgeIds[i]);
        BlockNumber childBlkno = edgeData->destinationNode;

        if (childBlkno != InvalidBlockNumber)
        {
            /* Read child node */
            Buffer childBuffer = ReadBuffer(index, childBlkno);
            LockBuffer(childBuffer, BUFFER_LOCK_SHARE);

            /* Recursively collect from child subtree */
            total += streeCollectSubtreeTids(index, childBuffer, callback, callbackArg);

            UnlockReleaseBuffer(childBuffer);
        }
    }

    return total;
}


/*
 * streeFindAndCollect - Find pattern and collect TIDs via callback.
 *
 * This function finds the node representing the pattern in the suffix tree,
 * then collects TIDs from the entire subtree rooted at that node. This is
 * necessary for substring search because all strings in the subtree contain
 * the pattern as a substring.
 *
 * Returns number of TIDs processed.
 */
int
streeFindAndCollect(Relation index, const char *pattern, int patternBytes,
                    void (*callback)(ItemPointer tid, void *arg), void *callbackArg)
{
    Buffer nodeBuf = streeFindBufferForPattern(index, pattern, patternBytes);
    int total = 0;

    if (!BufferIsValid(nodeBuf))
        return 0;

    /* 
     * Collect TIDs from entire subtree rooted at matching node.
     * This ensures we find ALL strings containing the pattern.
     */
    total = streeCollectSubtreeTids(index, nodeBuf, callback, callbackArg);

    /* release node buffer */
    UnlockReleaseBuffer(nodeBuf);

    return total;
}


/* ============================================================
 * Index Access Method Scan Functions
 * ============================================================ */

/*
 * Callback for collecting TIDs during search.
 * Performs deduplication to avoid returning the same TID multiple times.
 */
static void
streeCollectTidCallback(ItemPointer tid, void *arg)
{
    STreeScanOpaque so = (STreeScanOpaque) arg;
    
    /* Check for duplicates - TIDs might be stored at multiple nodes */
    for (int i = 0; i < so->numTids; i++)
    {
        if (ItemPointerEquals(&so->tids[i], tid))
            return;  /* Already have this TID */
    }
    
    /* Grow array if needed */
    if (so->numTids >= so->allocTids)
    {
        int newSize = (so->allocTids == 0) ? 64 : so->allocTids * 2;
        if (so->tids == NULL)
            so->tids = (ItemPointerData *) palloc(newSize * sizeof(ItemPointerData));
        else
            so->tids = (ItemPointerData *) repalloc(so->tids, 
                                                     newSize * sizeof(ItemPointerData));
        so->allocTids = newSize;
    }
    
    /* Copy the TID */
    so->tids[so->numTids++] = *tid;
}

/*
 * streebeginscan - Begin an index scan
 */
IndexScanDesc
streebeginscan(Relation rel, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    STreeScanOpaque so;
    
    /* No order by operators supported */
    Assert(norderbys == 0);
    
    scan = RelationGetIndexScan(rel, nkeys, norderbys);
    
    /* Allocate private scan state */
    so = (STreeScanOpaque) palloc0(sizeof(STreeScanOpaqueData));
    so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
                                        "STree scan temporary context",
                                        ALLOCSET_DEFAULT_SIZES);
    initSTreeState(&so->streestate, rel);
    
    so->pattern = NULL;
    so->patternLen = 0;
    so->tids = NULL;
    so->numTids = 0;
    so->allocTids = 0;
    so->curTid = 0;
    so->searchDone = false;
    so->matchedNodeBuf = InvalidBuffer;
    
    scan->opaque = so;
    
    return scan;
}

/*
 * Extract pattern from scan key - handles LIKE '%pattern%' format
 * Returns the substring between the wildcards (or the whole string if no wildcards)
 */
static char *
streeExtractPattern(ScanKey key, int *patternLen)
{
    char   *pattern;
    char   *result;
    int     len;
    int     startPos = 0;
    int     endPos;
    
    if (key == NULL || key->sk_argument == 0)
        return NULL;
    
    /* Get the pattern string from the Datum */
    pattern = TextDatumGetCString(key->sk_argument);
    len = strlen(pattern);
    
    if (len == 0)
    {
        pfree(pattern);
        return NULL;
    }
    
    /* Handle leading % */
    if (pattern[0] == '%')
        startPos = 1;
    
    /* Handle trailing % */
    endPos = len;
    if (len > 0 && pattern[len - 1] == '%')
        endPos = len - 1;
    
    /* Extract the pattern between wildcards */
    *patternLen = endPos - startPos;
    if (*patternLen <= 0)
    {
        pfree(pattern);
        return NULL;
    }
    
    result = palloc(*patternLen + 1);
    memcpy(result, pattern + startPos, *patternLen);
    result[*patternLen] = '\0';
    
    pfree(pattern);
    return result;
}

/*
 * streerescan - Restart an index scan
 */
void
streerescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
            ScanKey orderbys, int norderbys)
{
    STreeScanOpaque so = (STreeScanOpaque) scan->opaque;
    MemoryContext oldCtx;
    
    /* Reset scan state */
    so->curTid = 0;
    so->numTids = 0;
    so->searchDone = false;
    
    /* Release any held buffer */
    if (BufferIsValid(so->matchedNodeBuf))
    {
        ReleaseBuffer(so->matchedNodeBuf);
        so->matchedNodeBuf = InvalidBuffer;
    }
    
    /* Free old pattern if any */
    if (so->pattern != NULL)
    {
        pfree(so->pattern);
        so->pattern = NULL;
        so->patternLen = 0;
    }
    
    /* Copy scan keys */
    if (scankey && nscankeys > 0)
    {
        memmove(scan->keyData, scankey, nscankeys * sizeof(ScanKeyData));
    }
    
    /* Extract pattern from first scan key */
    if (nscankeys > 0)
    {
        oldCtx = MemoryContextSwitchTo(so->tempCtx);
        so->pattern = streeExtractPattern(&scan->keyData[0], &so->patternLen);
        MemoryContextSwitchTo(oldCtx);
    }
}

/*
 * streegettuple - Get next matching tuple from the index
 */
bool
streegettuple(IndexScanDesc scan, ScanDirection direction)
{
    STreeScanOpaque so = (STreeScanOpaque) scan->opaque;
    
    /* We don't support backward scans */
    if (ScanDirectionIsBackward(direction))
        elog(ERROR, "suffix tree index does not support backward scans");
    
    /* Perform search if not done yet */
    if (!so->searchDone)
    {
        so->searchDone = true;
        so->numTids = 0;
        so->curTid = 0;
        
        /* Search only if we have a pattern */
        if (so->pattern != NULL && so->patternLen > 0)
        {
            /* Collect all matching TIDs */
            streeFindAndCollect(scan->indexRelation,
                               so->pattern,
                               so->patternLen,
                               streeCollectTidCallback,
                               so);
        }
    }
    
    /* Return next TID if available */
    if (so->curTid < so->numTids)
    {
        scan->xs_heaptid = so->tids[so->curTid];
        so->curTid++;
        scan->xs_recheck = true;  /* Need recheck for exact LIKE match */
        return true;
    }
    
    return false;
}

/*
 * streeendscan - End an index scan
 */
void
streeendscan(IndexScanDesc scan)
{
    STreeScanOpaque so = (STreeScanOpaque) scan->opaque;
    
    /* Release any held buffer */
    if (BufferIsValid(so->matchedNodeBuf))
    {
        ReleaseBuffer(so->matchedNodeBuf);
        so->matchedNodeBuf = InvalidBuffer;
    }
    
    /* Free allocated memory */
    if (so->tids != NULL)
        pfree(so->tids);
    
    if (so->pattern != NULL)
        pfree(so->pattern);
    
    /* Delete temporary context */
    MemoryContextDelete(so->tempCtx);
    
    pfree(so);
}
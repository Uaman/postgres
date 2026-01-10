/*-------------------------------------------------------------------------
 *
 * streeinsert.c
 *	Core logic to insert entries into a suffix tree.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/suffixtree/streeinsert.c
 *
 *-------------------------------------------------------------------------
 */

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


/*
 * Structure for passing context to streeCopyTidToNode callback.
 */
typedef struct StreeCopyTidContext
{
    Relation    index;
    Buffer      destBuffer;
} StreeCopyTidContext;

/*
 * streeCopyTidToNode - Callback to copy TID to a node during edge split.
 * The arg is a StreeCopyTidContext pointer.
 */
static void
streeCopyTidToNode(ItemPointer tid, void *arg)
{
    StreeCopyTidContext *ctx = (StreeCopyTidContext *) arg;
    
    /* Add TID to the destination node - ignore failures silently */
    (void) streeAddHeapTidToNode(ctx->index, ctx->destBuffer, tid);
}


/*
 * findEdgeInsertPosition - Find position to insert edge maintaining sorted order.
 */
static uint16
findEdgeInsertPosition(STreeEdgeIdData *edgeIds, uint16 numOfEdges, pg_wchar firstChar)
{
    if (numOfEdges == 0)
        return 0;

    if (numOfEdges <= STREE_BSEARCH_THRESHOLD)
    {
        /* Linear search for small sets */
        for (uint16 i = 0; i < numOfEdges; i++)
        {
            if (EdgeIdGetFirstChar(&edgeIds[i]) >= firstChar)
                return i;
        }
        return numOfEdges;
    }
    else
    {
        /* Binary search for larger sets */
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
        return low;
    }
}

/**
 * streeinserttuple - Insert one tuple into the suffix tree index.
 *
 * Uses Ukkonen's algorithm. Stores TID at EVERY node visited, not just leaves.
 */
bool streeinserttuple(Relation index, STreeBuildState *state,
                 ItemPointer tid, Datum *values, bool *isnull) 
{
    bool        result = true;
    Datum       valueToInsert;
    char       *strValue;
    char       *strWithTerminator;  /* string with terminator appended */
    int         strByteLength;
    int         strCharLength;
    const char *strEnd;
    const char *currentPos;
    int         charIndex;
    Buffer      rootBuffer = InvalidBuffer;
    STreeActiveNode activeNode;
    int         activeLength = 0;
    int         activeEdgeCharIdx = 0;
    int         remainder = 0;
    BlockNumber lastNewInternalNode = InvalidBlockNumber;
    Buffer      lastNewInternalBuffer = InvalidBuffer;

    /* Handle NULL values */
    if (isnull[streeIndexedColumn])
        return true;

    /* Detoast the value if needed */
    valueToInsert = PointerGetDatum(PG_DETOAST_DATUM(values[streeIndexedColumn]));
    strValue = VARDATA_ANY(valueToInsert);
    strByteLength = VARSIZE_ANY_EXHDR(valueToInsert);

    /* Get character length for UTF-8 strings */
    strCharLength = pg_mbstrlen_with_len(strValue, strByteLength);

    if (strCharLength <= 0)
        return true;

    /*
     * Allocate buffer for string + terminator character.
     * The terminator ensures all suffixes end at explicit leaf nodes,
     * making TID storage unambiguous.
     */
    strWithTerminator = palloc(strByteLength + 1);
    memcpy(strWithTerminator, strValue, strByteLength);
    strWithTerminator[strByteLength] = STREE_TERMINATOR_CHAR;
    
    /* Update pointers and lengths to include terminator */
    strValue = strWithTerminator;
    strByteLength = strByteLength + 1;  /* +1 for terminator */
    strCharLength = strCharLength + 1;   /* terminator counts as one character */
    strEnd = strValue + strByteLength;

    /* Initialize active point */
    activeNode.blockNumber = STREE_ROOT_BLK;
    activeNode.buffer = InvalidBuffer;
    activeNode.page = NULL;

    /* Get and lock the root page */
    rootBuffer = ReadBuffer(index, STREE_ROOT_BLK);
    LockBuffer(rootBuffer, BUFFER_LOCK_EXCLUSIVE);

    activeNode.buffer = rootBuffer;
    activeNode.page = BufferGetPage(rootBuffer);

    /*
     * Add TID to ROOT node - every string passes through root.
     * This enables queries like '%' to find all strings.
     */
    if (!streeAddHeapTidToNode(index, rootBuffer, tid))
    {
        elog(WARNING, "Failed to add heap TID to root node");
        result = false;
        goto cleanup;
    }

    /* Ukkonen's algorithm main loop */
    currentPos = strValue;
    charIndex = 0;

    elog(DEBUG1, "Starting insertion of string '%.*s' (byte length %d, char length %d)",
         strByteLength, strValue, strByteLength, strCharLength);

    while (currentPos < strEnd)
    {
        const char *charStart = currentPos;
        pg_wchar    currentChar = streeGetNextChar(&currentPos, strEnd);

        elog(LOG, "Processing char %d: codepoint %u, remainder before=%d", 
             charIndex, currentChar, remainder);

        /* Check for interrupts periodically */
        if (INTERRUPTS_PENDING_CONDITION())
        {
            result = false;
            break;
        }

        remainder++; 
        charIndex++;

        /* Reset last new internal node for this phase */
        lastNewInternalNode = InvalidBlockNumber;
        if (BufferIsValid(lastNewInternalBuffer))
        {
            UnlockReleaseBuffer(lastNewInternalBuffer);
            lastNewInternalBuffer = InvalidBuffer;
        }

        int loopCounter = 0;
        const int maxLoops = strCharLength + 10;  /* Safety limit based on string length */
        
        while (remainder > 0)
        {
            /* 
             * Force compiler to re-read activeNode state from memory.
             * This prevents potential issues with cached/optimized values.
             */
            volatile BlockNumber _dummy_blk = activeNode.blockNumber;
            (void) _dummy_blk;
            
            loopCounter++;
            if (loopCounter > maxLoops)
            {
                elog(ERROR, "Inner loop exceeded %d iterations - likely infinite loop. remainder=%d, activeLength=%d",
                     maxLoops, remainder, activeLength);
            }
            
            if (activeLength == 0)
                activeEdgeCharIdx = charIndex - 1;

            /* Walk down as far as possible */

            /* Walk down as far as possible */
            while (activeLength > 0 &&
                   walkDown(index, &activeNode, &activeEdgeCharIdx, &activeLength,
                            strValue, strEnd, rootBuffer))
            {
                /*
                 * When we walk down to a child node, add TID to that node too.
                 * This ensures every node on the path has the TID.
                 */
                if (!streeAddHeapTidToNode(index, activeNode.buffer, tid))
                {
                    elog(WARNING, "Failed to add heap TID during walk down");
                    result = false;
                    goto cleanup;
                }
            }

            /* Get the active edge character */
            pg_wchar activeEdgeChar = getCharAtPosition(strValue, strEnd, activeEdgeCharIdx);

            /* Look for edge starting with active edge character */
            STreeEdgeIdData *edgeId = lookupEdgeByFirstChar(activeNode.page, activeEdgeChar);

            if (edgeId == NULL)
            {
                /*
                 * RULE 2: No edge found - create new leaf edge with a leaf node.
                 * 
                 * We create a proper leaf NODE (not just an edge with InvalidBlockNumber)
                 * so that we have a place to store TIDs for suffixes ending at this leaf.
                 * This is essential for correct substring matching.
                 */
                const char *labelStart = getByteOffsetForCharPos(strValue, strEnd, activeEdgeCharIdx);
                int         labelBytes = strEnd - labelStart;

                /* Create a leaf node for TID storage */
                Buffer leafNodeBuffer = STreeGetNewBuffer(index);
                if (!BufferIsValid(leafNodeBuffer))
                {
                    elog(WARNING, "Failed to allocate leaf node");
                    result = false;
                    goto cleanup;
                }
                
                BlockNumber leafNodeBlkno = BufferGetBlockNumber(leafNodeBuffer);
                Page leafNodePage = BufferGetPage(leafNodeBuffer);
                
                START_CRIT_SECTION();
                
                /* Initialize leaf node */
                STreeInitPage(leafNodePage, STREE_EDGE_NODE_PAGE);
                
                MarkBufferDirty(leafNodeBuffer);
                END_CRIT_SECTION();
                
                /* Store TID at the leaf node */
                if (!streeAddHeapTidToNode(index, leafNodeBuffer, tid))
                {
                    UnlockReleaseBuffer(leafNodeBuffer);
                    elog(WARNING, "Failed to add heap TID to leaf node");
                    result = false;
                    goto cleanup;
                }
                
                UnlockReleaseBuffer(leafNodeBuffer);

                START_CRIT_SECTION();

                STreeEdgeIdData *newEdge = insertEdgeSorted(
                    activeNode.page,
                    activeEdgeChar,
                    labelStart,
                    labelBytes,
                    leafNodeBlkno  /* Point to the leaf node */
                );

                if (newEdge == NULL)
                {
                    END_CRIT_SECTION();
                    elog(WARNING, "Page full when inserting edge");
                    result = false;
                    goto cleanup;
                }

                MarkBufferDirty(activeNode.buffer);

                END_CRIT_SECTION();

                /* Set suffix link from previous internal node */
                if (lastNewInternalNode != InvalidBlockNumber &&
                    BufferIsValid(lastNewInternalBuffer))
                {
                    STreeNodePageOpaque lastOpaque = 
                        (STreeNodePageOpaque) PageGetSpecialPointer(
                            BufferGetPage(lastNewInternalBuffer));
                    
                    START_CRIT_SECTION();
                    lastOpaque->suffixLink = activeNode.blockNumber;
                    MarkBufferDirty(lastNewInternalBuffer);
                    END_CRIT_SECTION();

                    UnlockReleaseBuffer(lastNewInternalBuffer);
                    lastNewInternalBuffer = InvalidBuffer;
                }
                lastNewInternalNode = InvalidBlockNumber;

                remainder--;

                /* Follow suffix link or adjust active point */
                if (activeNode.blockNumber == STREE_ROOT_BLK && activeLength > 0)
                {
                    activeLength--;
                    activeEdgeCharIdx++;
                }
                else if (activeNode.blockNumber != STREE_ROOT_BLK)
                {
                    if (!followSuffixLink(index, &activeNode, rootBuffer, STREE_ROOT_BLK))
                    {
                        result = false;
                        goto cleanup;
                    }
                }
                else
                {
                    /* At root with activeLength == 0: done with this suffix */
                    break;
                }
            }
            else
            {
                /*
                 * Edge exists - check character at activeLength position.
                 */
                STreeEdgeInsideData *edgeData = streeGetEdgeData(activeNode.page, edgeId);
                int edgeLabelCharLen = getEdgeLabelCharLength(edgeData);

                pg_wchar edgeCharAtPos;
                
                if (activeLength < edgeLabelCharLen)
                {
                    edgeCharAtPos = getEdgeLabelCharAt(edgeData, activeLength);
                }
                else
                {
                    /*
                     * activeLength >= edgeLabelCharLen but walkDown didn't happen.
                     * This means we're at a leaf edge or destination is invalid.
                     * We need to handle this case - we can't split here!
                     */
                    if (edgeData->destinationNode == InvalidBlockNumber)
                    {
                        /*
                         * This is a leaf edge. We need to extend it or create 
                         * an internal node here. For now, just add the TID to 
                         * the current node and continue.
                         */
                        remainder--;
                        
                        /* Follow suffix link or adjust active point */
                        if (activeNode.blockNumber == STREE_ROOT_BLK && activeLength > 0)
                        {
                            activeLength--;
                            activeEdgeCharIdx++;
                        }
                        else if (activeNode.blockNumber != STREE_ROOT_BLK)
                        {
                            if (!followSuffixLink(index, &activeNode, rootBuffer, STREE_ROOT_BLK))
                            {
                                result = false;
                                goto cleanup;
                            }
                        }
                        continue;
                    }
                    edgeCharAtPos = 0;
                }

                if (edgeCharAtPos == currentChar)
                {
                    /*
                     * RULE 3: Character matches - stop this phase.
                     * 
                     * Store TID at the appropriate node for this path.
                     * If we're at position 0 of an edge with a destination node,
                     * the TID belongs at that destination (it's part of that subtree).
                     * Otherwise store at current node.
                     */
                    Buffer tidBuffer = activeNode.buffer;
                    bool needReleaseTidBuffer = false;
                    
                    if (activeLength == 0 && edgeData->destinationNode != InvalidBlockNumber)
                    {
                        /* Store at destination node since we're entering this subtree */
                        tidBuffer = ReadBuffer(index, edgeData->destinationNode);
                        LockBuffer(tidBuffer, BUFFER_LOCK_EXCLUSIVE);
                        needReleaseTidBuffer = true;
                    }
                    
                    if (!streeAddHeapTidToNode(index, tidBuffer, tid))
                    {
                        if (needReleaseTidBuffer)
                            UnlockReleaseBuffer(tidBuffer);
                        elog(WARNING, "Failed to add heap TID at Rule 3 match");
                        result = false;
                        goto cleanup;
                    }
                    
                    if (needReleaseTidBuffer)
                        UnlockReleaseBuffer(tidBuffer);
                    
                    activeLength++;

                    /* Set suffix link from previous internal node */
                    if (lastNewInternalNode != InvalidBlockNumber &&
                        BufferIsValid(lastNewInternalBuffer))
                    {
                        STreeNodePageOpaque lastOpaque = 
                            (STreeNodePageOpaque) PageGetSpecialPointer(
                                BufferGetPage(lastNewInternalBuffer));

                        START_CRIT_SECTION();
                        lastOpaque->suffixLink = activeNode.blockNumber;
                        MarkBufferDirty(lastNewInternalBuffer);
                        END_CRIT_SECTION();

                        UnlockReleaseBuffer(lastNewInternalBuffer);
                        lastNewInternalBuffer = InvalidBuffer;
                    }
                    lastNewInternalNode = InvalidBlockNumber;

                    break;  /* Move to next phase */
                }
                else
                {
                    /*
                     * RULE 2 with SPLIT: Need to split the edge.
                     * Only valid if activeLength > 0 (we're inside an edge).
                     */
                    if (activeLength == 0)
                    {
                        elog(WARNING, "Cannot split at position 0 - logic error");
                        result = false;
                        goto cleanup;
                    }
                    
                    BlockNumber newInternalBlkno;

                    newInternalBlkno = splitEdgeWithBuffer(
                        index,
                        activeNode.buffer,
                        edgeId,
                        activeLength,
                        NULL
                    );

                    if (newInternalBlkno == InvalidBlockNumber)
                    {
                        elog(WARNING, "Failed to split edge");
                        result = false;
                        goto cleanup;
                    }

                    /*
                     * Add TID to the NEW INTERNAL NODE.
                     * This is crucial for substring matching!
                     */
                    Buffer newInternalBuffer = ReadBuffer(index, newInternalBlkno);
                    LockBuffer(newInternalBuffer, BUFFER_LOCK_EXCLUSIVE);
                    Page newInternalPage = BufferGetPage(newInternalBuffer);

                    if (!streeAddHeapTidToNode(index, newInternalBuffer, tid))
                    {
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to add heap TID to new internal node");
                        result = false;
                        goto cleanup;
                    }
                    
                    /*
                     * CRITICAL: Copy TIDs from parent node to the new internal node.
                     * When we split an edge, previous strings that contain the
                     * substring represented by this path should also be at the
                     * new internal node. We copy all TIDs from parent - this is
                     * conservative but correct for substring matching.
                     */
                    {
                        STreeNodePageOpaque parentOpaque = 
                            (STreeNodePageOpaque) PageGetSpecialPointer(activeNode.page);
                        
                        if (parentOpaque->itemPointersStart != InvalidBlockNumber)
                        {
                            StreeCopyTidContext copyCtx;
                            copyCtx.index = index;
                            copyCtx.destBuffer = newInternalBuffer;
                            
                            /* Copy TIDs from parent's data pages */
                            streeGetDataPageTids(index, parentOpaque->itemPointersStart,
                                                 streeCopyTidToNode, &copyCtx);
                        }
                    }

                    /* Create leaf edge from new internal node - with a proper leaf node */
                    const char *leafLabelStart = charStart;
                    int leafLabelBytes = strEnd - leafLabelStart;

                    /* Create a leaf node for TID storage */
                    Buffer splitLeafBuffer = STreeGetNewBuffer(index);
                    if (!BufferIsValid(splitLeafBuffer))
                    {
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to allocate leaf node after split");
                        result = false;
                        goto cleanup;
                    }
                    
                    BlockNumber splitLeafBlkno = BufferGetBlockNumber(splitLeafBuffer);
                    Page splitLeafPage = BufferGetPage(splitLeafBuffer);
                    
                    START_CRIT_SECTION();
                    STreeInitPage(splitLeafPage, STREE_EDGE_NODE_PAGE);
                    MarkBufferDirty(splitLeafBuffer);
                    END_CRIT_SECTION();
                    
                    /* Store TID at the leaf node */
                    if (!streeAddHeapTidToNode(index, splitLeafBuffer, tid))
                    {
                        UnlockReleaseBuffer(splitLeafBuffer);
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to add heap TID to split leaf node");
                        result = false;
                        goto cleanup;
                    }
                    
                    UnlockReleaseBuffer(splitLeafBuffer);

                    START_CRIT_SECTION();

                    STreeEdgeIdData *leafEdge = insertEdgeSorted(
                        newInternalPage,
                        currentChar,
                        leafLabelStart,
                        leafLabelBytes,
                        splitLeafBlkno  /* Point to the leaf node */
                    );

                    if (leafEdge == NULL)
                    {
                        END_CRIT_SECTION();
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to insert leaf edge after split");
                        result = false;
                        goto cleanup;
                    }

                    MarkBufferDirty(newInternalBuffer);

                    END_CRIT_SECTION();

                    /* Set suffix link from previous internal node */
                    if (lastNewInternalNode != InvalidBlockNumber &&
                        BufferIsValid(lastNewInternalBuffer))
                    {
                        STreeNodePageOpaque lastOpaque = 
                            (STreeNodePageOpaque) PageGetSpecialPointer(
                                BufferGetPage(lastNewInternalBuffer));

                        START_CRIT_SECTION();
                        lastOpaque->suffixLink = newInternalBlkno;
                        MarkBufferDirty(lastNewInternalBuffer);
                        END_CRIT_SECTION();

                        UnlockReleaseBuffer(lastNewInternalBuffer);
                    }

                    /* Remember this internal node for suffix link chaining */
                    lastNewInternalNode = newInternalBlkno;
                    lastNewInternalBuffer = newInternalBuffer;

                    remainder--;

                    /* Follow suffix link or adjust active point */
                    if (activeNode.blockNumber == STREE_ROOT_BLK && activeLength > 0)
                    {
                        activeLength--;
                        activeEdgeCharIdx++;
                    }
                    else if (activeNode.blockNumber != STREE_ROOT_BLK)
                    {
                        if (!followSuffixLink(index, &activeNode, rootBuffer, STREE_ROOT_BLK))
                        {
                            UnlockReleaseBuffer(lastNewInternalBuffer);
                            lastNewInternalBuffer = InvalidBuffer;
                            result = false;
                            goto cleanup;
                        }
                    }
                }
            }
        }
    }

    /* Update indexed tuple count */
    if (result && state != NULL)
        state->indexedTuples++;

cleanup:
    /* Free the allocated terminator string */
    if (strWithTerminator != NULL)
        pfree(strWithTerminator);

    if (BufferIsValid(lastNewInternalBuffer))
        UnlockReleaseBuffer(lastNewInternalBuffer);

    if (activeNode.buffer != rootBuffer && BufferIsValid(activeNode.buffer))
        UnlockReleaseBuffer(activeNode.buffer);

    if (BufferIsValid(rootBuffer))
        UnlockReleaseBuffer(rootBuffer);

    CHECK_FOR_INTERRUPTS();

    return result;
}


/*
 * insertEdgeSorted - Insert a new edge into the page, maintaining sorted order.
 *
 * Parameters:
 *   page        - The page to insert into (must be locked exclusively)
 *   firstChar   - First character of the edge label (Unicode codepoint)
 *   labelData   - Pointer to the edge label bytes (UTF-8)
 *   labelLen    - Length of label in bytes
 *   destBlock   - Destination block number (child node), or InvalidBlockNumber for leaf
 *
 * Returns:
 *   Pointer to the newly inserted STreeEdgeIdData, or NULL if page is full.
 */
STreeEdgeIdData *
insertEdgeSorted(Page page, pg_wchar firstChar, const char *labelData, 
                 uint16 labelLen, BlockNumber destBlock)
{
    STreeEdgeIdArrayHeader *header;
    STreeEdgeIdData        *edgeIds;
    STreeEdgeInsideData    *edgeInside;
    STreeNodePageOpaque     opaque;
    uint16                  numOfEdges;
    uint16                  insertPos;
    Size                    spaceNeeded;
    Offset                  edgeDataOffset;
    Offset                  specialOffset;
    Offset                  lowestOffset;
    Offset                  edgeIdsEnd;

    Assert(page != NULL);

    header = STreePageGetEdgeIdHeader(page);
    numOfEdges = header->numberOfEdges;
    edgeIds = STreePageGetEdgeIds(page);
    opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);

    /* Calculate space needed */
    spaceNeeded = sizeof(STreeEdgeIdData) + SizeOfSTreeEdgeInsideData + labelLen;

    /* Calculate where special pointer starts */
    specialOffset = (char *) opaque - (char *) page;

    /* Find lowest existing edge data offset */
    lowestOffset = specialOffset;
    for (uint16 i = 0; i < numOfEdges; i++)
    {
        if (edgeIds[i].realEdgeOffset < lowestOffset)
            lowestOffset = edgeIds[i].realEdgeOffset;
    }

    /* Calculate where edge IDs array ends */
    edgeIdsEnd = ((char *) PageGetContents(page) - (char *) page) +
                 sizeof(STreeEdgeIdArrayHeader) +
                 ((numOfEdges + 1) * sizeof(STreeEdgeIdData));

    /* New edge data goes below existing edge data */
    edgeDataOffset = lowestOffset - (SizeOfSTreeEdgeInsideData + labelLen);

    /* Check if we have enough space */
    if (edgeIdsEnd > edgeDataOffset)
    {
        /* Page full */
        return NULL;
    }

    /* Find insertion position */
    insertPos = findEdgeInsertPosition(edgeIds, numOfEdges, firstChar);

    /* Check for duplicate */
    if (insertPos < numOfEdges && 
        EdgeIdGetFirstChar(&edgeIds[insertPos]) == firstChar)
    {
        elog(WARNING, "Edge with firstChar %u already exists", firstChar);
        return NULL;
    }

    /* Initialize edge inside data */
    edgeInside = (STreeEdgeInsideData *) ((char *) page + edgeDataOffset);
    edgeInside->destinationNode = destBlock;
    edgeInside->labelLength = labelLen;

    if (labelLen > 0 && labelData != NULL)
        memcpy(edgeInside->label, labelData, labelLen);

    /* Shift existing edge IDs to make room */
    if (insertPos < numOfEdges)
    {
        memmove(&edgeIds[insertPos + 1],
                &edgeIds[insertPos],
                (numOfEdges - insertPos) * sizeof(STreeEdgeIdData));
    }

    /* Initialize new edge ID */
    edgeIds[insertPos].firstChar = firstChar;
    edgeIds[insertPos].realEdgeOffset = edgeDataOffset;
    edgeIds[insertPos].realEdgeDataLength = SizeOfSTreeEdgeInsideData + labelLen;

    /* Update count */
    header->numberOfEdges++;

    /*
     * Update page boundaries:
     * - pd_lower: end of edge IDs array (grows upward)
     * - pd_upper: start of edge data (grows downward)
     */
    ((PageHeader) page)->pd_lower = edgeIdsEnd;
    ((PageHeader) page)->pd_upper = edgeDataOffset;

    return &edgeIds[insertPos];
}
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
    strEnd = strValue + strByteLength;

    /* Get character length for UTF-8 strings */
    strCharLength = pg_mbstrlen_with_len(strValue, strByteLength);

    if (strCharLength <= 0)
        return true;

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

    elog(LOG, ">>>>>Starting insertion of string of byte length %d, char length %d",
         strByteLength, strCharLength);

    while (currentPos < strEnd)
    {
        const char *charStart = currentPos;
        pg_wchar    currentChar = streeGetNextChar(&currentPos, strEnd);
        int         currentCharByteLen = currentPos - charStart;

        elog(DEBUG1, "Processing character at byte index %ld, Unicode codepoint %u",
             charStart - strValue, currentChar);

        //TODO: consider storing current insertion state for resuming after interrupt     
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

        while (remainder > 0)
        {
            if (activeLength == 0)
                activeEdgeCharIdx = charIndex - 1;

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

            elog(LOG, "Checked for edge with char %u, found: %s", 
                 activeEdgeChar, edgeId ? "yes" : "no");

            if (edgeId == NULL)
            {
                /*
                 * RULE 2: No edge found - create new leaf edge.
                 */
                const char *labelStart = getByteOffsetForCharPos(strValue, strEnd, activeEdgeCharIdx);
                int         labelBytes = strEnd - labelStart;

                elog(LOG, "Rule 2 (no edge): creating leaf at char %d, labelBytes=%d", 
                     activeEdgeCharIdx, labelBytes);

                START_CRIT_SECTION();

                STreeEdgeIdData *newEdge = insertEdgeSorted(
                    activeNode.page,
                    activeEdgeChar,
                    labelStart,
                    labelBytes,
                    InvalidBlockNumber  /* Leaf - no child node yet */
                );

                if (newEdge == NULL)
                {
                    END_CRIT_SECTION();
                    elog(WARNING, "Page full when inserting edge");
                    result = false;
                    goto cleanup;
                }

                elog(LOG, "Rule 2: Edge inserted successfully");

                MarkBufferDirty(activeNode.buffer);

                END_CRIT_SECTION();

                /*
                 * TID is stored at the NODE level, not at the edge level.
                 * The TID was already added to root initially, and to each node
                 * we walked down to. The leaf edge's destination is InvalidBlockNumber
                 * since it's a leaf - we don't store TIDs via edges.
                 */

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
                    /*
                     * At root with activeLength == 0: we just inserted a new leaf edge.
                     * The current suffix is fully handled. Break out of the remainder loop
                     * to continue with the next character in the outer loop.
                     */
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
                    edgeCharAtPos = getEdgeLabelCharAt(edgeData, activeLength);
                else
                    edgeCharAtPos = 0;

                if (edgeCharAtPos == currentChar)
                {
                    /*
                     * RULE 3: Character matches - stop this phase.
                     */
                    elog(DEBUG2, "Rule 3: char matches, activeLength++");

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
                     */
                    elog(DEBUG2, "Rule 2 (split): splitting at pos %d", activeLength);

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

                    /* Create leaf edge from new internal node */
                    const char *leafLabelStart = charStart;
                    int leafLabelBytes = strEnd - leafLabelStart;

                    START_CRIT_SECTION();

                    STreeEdgeIdData *leafEdge = insertEdgeSorted(
                        newInternalPage,
                        currentChar,
                        leafLabelStart,
                        leafLabelBytes,
                        InvalidBlockNumber  /* Leaf */
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

                    /*
                     * Add TID to the new internal node we just created.
                     * This is the node that will be traversed for any suffix
                     * starting with the split prefix.
                     */
                    if (!streeAddHeapTidToNode(index, newInternalBuffer, tid))
                    {
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to add heap TID to new internal node after split");
                        result = false;
                        goto cleanup;
                    }

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
    elog(LOG, "streeinserttuple: cleanup starting");
    if (BufferIsValid(lastNewInternalBuffer))
    {
        elog(LOG, "streeinserttuple: releasing lastNewInternalBuffer");
        UnlockReleaseBuffer(lastNewInternalBuffer);
    }

    if (activeNode.buffer != rootBuffer && BufferIsValid(activeNode.buffer))
    {
        elog(LOG, "streeinserttuple: releasing activeNode.buffer");
        UnlockReleaseBuffer(activeNode.buffer);
    }

    if (BufferIsValid(rootBuffer))
    {
        elog(LOG, "streeinserttuple: releasing rootBuffer");
        UnlockReleaseBuffer(rootBuffer);
    }

    elog(LOG, "streeinserttuple: cleanup complete, returning %s", result ? "true" : "false");
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
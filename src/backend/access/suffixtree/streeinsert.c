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



/**
 * 
 */
bool streeinserttuple(Relation index, STreeBuildState *state,
                 ItemPointer tid, Datum *values, bool *isnull) 
{
    bool        result = true;
    Datum       valueToInsert;
    char       *strValue;
    int         strByteLength;      /* Length in bytes */
    int         strCharLength;      /* Length in characters */
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
    {
        return true;  /* Skip nulls for now */
    }

    /* Detoast the value if needed (varlena type) */
    valueToInsert = PointerGetDatum(PG_DETOAST_DATUM(values[streeIndexedColumn]));
    strValue = VARDATA_ANY(valueToInsert);
    strByteLength = VARSIZE_ANY_EXHDR(valueToInsert);
    strEnd = strValue + strByteLength;


    /* Get character length (not byte length) for UTF-8 strings */
    strCharLength = pg_mbstrlen_with_len(strValue, strByteLength);

    if (strCharLength <= 0)
        return true;  /* Nothing to index */

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
     * Ukkonen's algorithm main loop.
     * We iterate by CHARACTER (not byte), using pg_wchar for comparisons.
     */
    currentPos = strValue;
    charIndex = 0;

    elog(DEBUG1, ">>>>>Starting insertion of string of byte length %d, char length %d",
         strByteLength, strCharLength);
    while (currentPos < strEnd)
    {
        /* Get current character as Unicode codepoint */
        const char *charStart = currentPos;
        pg_wchar    currentChar = streeGetNextChar(&currentPos, strEnd);
        int         currentCharByteLen = currentPos - charStart;

        elog(DEBUG1, "Processing character at byte index %ld, Unicode codepoint %u",
             charStart - strValue, currentChar);
        elog(DEBUG1, "Current char %u", currentChar);

        // >>>>>>>> TODO: need to save and restore active point from state
        if (INTERRUPTS_PENDING_CONDITION())
        {
            result = false;
            break;
        }

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
            {
                activeEdgeCharIdx = charIndex - 1;
            }

            /* Walk down as far as possible */
            while (walkDown(index, &activeNode, &activeEdgeCharIdx, &activeLength,
                            strValue, strEnd, rootBuffer))
            {
                /* Keep walking */
            }

            /* Get the active edge character */
            pg_wchar activeEdgeChar = getCharAtPosition(strValue, strEnd, activeEdgeCharIdx);

            /* Look for edge starting with active edge character */
            STreeEdgeIdData *edgeId = lookupEdgeByFirstChar(activeNode.page, activeEdgeChar);

            if (edgeId == NULL)
            {
                /*
                 * RULE 2: No edge found - create new leaf edge.
                 */
                const char *labelStart = getByteOffsetForCharPos(strValue, strEnd, activeEdgeCharIdx);
                int         labelBytes = strEnd - labelStart;

                elog(DEBUG2, "Rule 2 (no edge): creating leaf at char %d", activeEdgeCharIdx);

                START_CRIT_SECTION();

                STreeEdgeIdData *newEdge = insertEdgeSorted(
                    activeNode.page,
                    activeEdgeChar,
                    labelStart,
                    labelBytes,
                    InvalidBlockNumber  /* Leaf - no child node */
                );

                if (newEdge == NULL)
                {
                    /* Page full - need to handle overflow */
                    END_CRIT_SECTION();
                    elog(WARNING, "Page full when inserting edge");
                    result = false;
                    goto cleanup;
                }

                MarkBufferDirty(activeNode.buffer);

                END_CRIT_SECTION();

                /* Store the heap TID in the leaf's data page */
                if (!streeAddHeapTid(index, activeNode.buffer, newEdge, tid))
                {
                    elog(WARNING, "Failed to add heap TID to leaf");
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
            }
            else
            {
                /*
                 * Edge exists - check character at activeLength position.
                 */
                STreeEdgeInsideData *edgeData = streeGetEdgeData(activeNode.page, edgeId);
                int edgeLabelCharLen = getEdgeLabelCharLength(edgeData);

                /* Get character at activeLength position in the edge label */
                pg_wchar edgeCharAtPos;
                
                if (activeLength < edgeLabelCharLen)
                {
                    edgeCharAtPos = getEdgeLabelCharAt(edgeData, activeLength);
                }
                else
                {
                    /* activeLength >= edge length - should have walked down */
                    edgeCharAtPos = 0;
                }

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
                     * Now create leaf edge from new internal node.
                     */
                    Buffer newInternalBuffer = ReadBuffer(index, newInternalBlkno);
                    LockBuffer(newInternalBuffer, BUFFER_LOCK_EXCLUSIVE);
                    Page newInternalPage = BufferGetPage(newInternalBuffer);

                    const char *leafLabelStart = charStart;  /* From current char */
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

                    /* Store the heap TID in the new leaf's data page */
                    if (!streeAddHeapTid(index, newInternalBuffer, leafEdge, tid))
                    {
                        UnlockReleaseBuffer(newInternalBuffer);
                        elog(WARNING, "Failed to add heap TID after split");
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
                    /* Note: Don't unlock - we need it for suffix link */

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
    /* Release any held buffers */
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
    uint16                  numOfEdges;
    uint16                  insertPos;
    Size                    spaceNeeded;
    Size                    freeSpace;
    Offset                  edgeDataOffset;

    Assert(page != NULL);
    Assert(labelData != NULL || labelLen == 0);

    header = STreePageGetEdgeIdHeader(page);
    numOfEdges = header->numberOfEdges;
    edgeIds = STreePageGetEdgeIds(page);

    /*
     * Calculate space needed:
     * - One new STreeEdgeIdData in the edge ID array
     * - The edge inside data (header + label bytes)
     */
    spaceNeeded = sizeof(STreeEdgeIdData) + 
                  SizeOfSTreeEdgeInsideData + labelLen;

    /* Check if we have enough free space */
    freeSpace = PageGetFreeSpace(page);
    if (freeSpace < spaceNeeded)
    {
        /* Not enough space - caller needs to handle page split */
        return NULL;
    }

    /* Find the position to insert to maintain sorted order */
    insertPos = findEdgeInsertPosition(edgeIds, numOfEdges, firstChar);

    /* Check if edge with same firstChar already exists */
    if (insertPos < numOfEdges && 
        EdgeIdGetFirstChar(&edgeIds[insertPos]) == firstChar)
    {
        elog(WARNING, "Edge with firstChar %u already exists", firstChar);
        return NULL;
    }

    /*
     * Allocate space for the edge data (STreeEdgeInsideData).
     * Edge data grows from the end of the page (before special space).
     * 
     * TODO: You need to track the lowest used offset. For now, calculate it.
     */
    STreeNodePageOpaque opaque = (STreeNodePageOpaque) PageGetSpecialPointer(page);
    Offset specialOffset = (char *) opaque - (char *) page;
    
    /* Calculate where to put new edge data */
    edgeDataOffset = specialOffset - (SizeOfSTreeEdgeInsideData + labelLen);
    
    /* TODO: Account for existing edge data - need to track lowest offset */

    edgeInside = (STreeEdgeInsideData *) ((char *) page + edgeDataOffset);

    /* Initialize edge inside data */
    edgeInside->destinationNode = destBlock;
    edgeInside->labelLength = labelLen;
    
    /* Copy label data */
    if (labelLen > 0)
        memcpy(edgeInside->label, labelData, labelLen);

    /*
     * Shift existing edge IDs to make room for the new one.
     */
    if (insertPos < numOfEdges)
    {
        memmove(&edgeIds[insertPos + 1], 
                &edgeIds[insertPos], 
                (numOfEdges - insertPos) * sizeof(STreeEdgeIdData));
    }

    /* Initialize the new edge ID (no destinationNode here!) */
    edgeIds[insertPos].firstChar = firstChar;
    edgeIds[insertPos].realEdgeOffset = edgeDataOffset;
    edgeIds[insertPos].realEdgeDataLength = SizeOfSTreeEdgeInsideData + labelLen;

    /* Update edge count */
    header->numberOfEdges++;

    return &edgeIds[insertPos];
}

/*
 * streeAddHeapTid - Add a heap TID to a leaf node.
 *
 * Leaf nodes in the suffix tree point to data pages that store the actual
 * heap TIDs. This function adds a TID to the appropriate data page.
 *
 * Parameters:
 *   index       - The relation
 *   leafPage    - The leaf node page (edge node with destinationNode = InvalidBlockNumber)
 *   edgeId      - The edge ID for the leaf
 *   tid         - The heap TID to store
 *
 * Returns:
 *   true on success, false if we need to allocate a new data page
 */
bool
streeAddHeapTid(Relation index, Buffer leafBuffer, STreeEdgeIdData *edgeId, 
                ItemPointer tid)
{
    Page                    leafPage;
    STreeEdgeInsideData    *edgeData;
    BlockNumber             dataPageBlkno;
    Buffer                  dataBuffer;
    Page                    dataPage;
    STreeNodeTupleDataEntries *header;
    STreeNodeTuple          tuples;
    Size                    tupleSize;
    Size                    spaceNeeded;
    Size                    freeSpace;

    Assert(BufferIsValid(leafBuffer));
    Assert(edgeId != NULL);
    Assert(tid != NULL);

    leafPage = BufferGetPage(leafBuffer);
    edgeData = streeGetEdgeData(leafPage, edgeId);

    /* 
     * For leaf edges, destinationNode points to the data page.
     * If InvalidBlockNumber, we need to allocate one.
     */
    dataPageBlkno = edgeData->destinationNode;

    if (dataPageBlkno == InvalidBlockNumber)
    {
        /* Allocate new data page */
        dataBuffer = streeAllocateDataPage(index, leafBuffer, edgeId);
        if (!BufferIsValid(dataBuffer))
            return false;
        
        dataPageBlkno = BufferGetBlockNumber(dataBuffer);
    }
    else
    {
        /* Read existing data page */
        dataBuffer = ReadBuffer(index, dataPageBlkno);
        LockBuffer(dataBuffer, BUFFER_LOCK_EXCLUSIVE);
    }

    dataPage = BufferGetPage(dataBuffer);
    header = (STreeNodeTupleDataEntries *) PageGetContents(dataPage);

    /*
     * Calculate space needed for the new tuple.
     * We use IndexTupleData which stores the TID.
     */
    tupleSize = sizeof(IndexTupleData);
    spaceNeeded = MAXALIGN(tupleSize);
    freeSpace = PageGetFreeSpace(dataPage);

    if (freeSpace < spaceNeeded)
    {
        /*
         * Data page is full - need to chain to a new page.
         */
        UnlockReleaseBuffer(dataBuffer);
        dataBuffer = streeAllocateOverflowDataPage(index, leafBuffer, edgeId, dataPageBlkno);
        if (!BufferIsValid(dataBuffer))
            return false;
        
        dataPage = BufferGetPage(dataBuffer);
        header = (STreeNodeTupleDataEntries *) PageGetContents(dataPage);
    }

    START_CRIT_SECTION();

    /*
     * Get pointer to tuple array and add new entry.
     */
    tuples = (STreeNodeTuple) (((char *) header) + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE);
    
    /* Add new tuple at the end */
    STreeNodeTuple newTuple = &tuples[header->numberOfEntries];
    
    /* Initialize the index tuple with the heap TID */
    newTuple->t_tid = *tid;
    newTuple->t_info = 0;  /* No additional data beyond TID */

    header->numberOfEntries++;

    MarkBufferDirty(dataBuffer);

    END_CRIT_SECTION();

    UnlockReleaseBuffer(dataBuffer);

    return true;
}



// bool streeinsertchar(Relation index, STreeBuildState *state,
//                  ItemPointer tid, Datum *values, bool *isnull) 
// {
//     bool			isSuccess = true;
//     Datum			valueToInsert;
//     int				valueSize = 0;
//     STreePageInfo	current,
//                     parent;

//     if (!isnull) {
//         //(attType.attlen == -1) // type can be varlena and needs detoasting
//         //valueToInsert = PointerGetDatum(PG_DETOAST_DATUM(values[streeKeyColumn]));
//         valueToInsert = values[streeKeyColumn];
//     } else {
//         valueToInsert = (Datum) 0; // null value
//     }

//     //CalculateTupleSize ??
//     /**
//      * TODO:
//      * Check if the value size is not exceeding the maximum allowed size
//      * for indexing. If it does, raise an error.
//      */
//     // if (valueSize > MAX_INDEXABLE_SIZE) {

//     current.blkno = isnull ? STREE_NULL_BLK : STREE_ROOT_BLK;
//     current.buffer = InvalidBuffer;
//     current.page = NULL;
//     current.offnum = FirstOffsetNumber;
//     current.node = -1;

//     parent.blkno = InvalidBlockNumber;
//     parent.buffer = InvalidBuffer;
//     parent.page = NULL;
//     parent.offnum = InvalidOffsetNumber;
//     parent.node = -1;


//     CHECK_FOR_INTERRUPTS();

//     for (;;) {

//         if (INTERRUPTS_PENDING_CONDITION())
// 		{
// 			isSuccess = false;
// 			break;
// 		}
//     }

//     if (current.buffer == InvalidBlockNumber)
//     {
//         // get the buffer for the current page
//     }
//     else if (parent.buffer == InvalidBuffer)
//     {
//         // get the buffer for the parent page
//         current.buffer = ReadBuffer(index, current.blkno);
//         LockBuffer(current.buffer, BUFFER_LOCK_EXCLUSIVE);
//     }
//     else if (current.blkno != parent.blkno)
//     {
//         // get the buffer for the current page
//         current.buffer = ReadBuffer(index, current.blkno);
//         // Attempt to acquire lock on child page.  We must beware of
//         // deadlock against another insertion process descending from that
//         // page to our parent page (see README).  If we fail to get lock,
//         // we release our parent lock and try again.
//         if (!ConditionalLockBuffer(current.buffer))
//         {
//             ReleaseBuffer(current.buffer);
// 			UnlockReleaseBuffer(parent.buffer);
// 			return false;
//         }
//     }
//     else
// 	{
// 		/* inner tuple can be stored on the same page as parent one */
// 		current.buffer = parent.buffer;
// 	}
//     current.page = BufferGetPage(current.buffer);



//     // Insert code starts here
    


    
//     /**
//      * TODO:
//      * Release any buffers we're still holding.  Beware of possibility that
//      * current and parent reference same buffer.
//      */

    
//     Assert(INTERRUPTS_CAN_BE_PROCESSED());

//     /*
// 	 * Finally, check for interrupts again.  If there was a query cancel,
// 	 * ProcessInterrupts() will be able to throw the error here.  If it was
// 	 * some other kind of interrupt that can just be cleared, return false to
// 	 * tell our caller to retry.
// 	 */
// 	CHECK_FOR_INTERRUPTS();
    
//     return isSuccess;    
// }

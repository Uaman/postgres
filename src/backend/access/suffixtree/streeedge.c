/*-------------------------------------------------------------------------
 *
 * streeedge.c
 *	Core logic for managing suffix tree edges.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/suffixtree/streeedge.c
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
 * splitEdge - Split an existing edge at a given position.
 *
 * This is used in Ukkonen's algorithm when we need to create a new internal
 * node in the middle of an existing edge (Rule 2 with split).
 *
 * Before split:
 *   parentNode --[fullLabel]--> destNode
 *
 * After split (at position splitPos):
 *   parentNode --[prefix]--> newInternalNode --[suffix]--> destNode
 *
 * Parameters:
 *   index       - The relation (for allocating new pages)
 *   parentPage  - The page containing the edge to split (must be locked exclusively)
 *   edgeId      - Pointer to the edge ID to split
 *   splitPos    - Character position where to split (in characters, not bytes)
 *
 * Returns:
 *   BlockNumber of the newly created internal node, or InvalidBlockNumber on failure.
 *
 * Note: The caller is responsible for:
 *   1. Creating a new leaf edge from the new internal node
 *   2. Setting up suffix links
 */
BlockNumber
splitEdge(Relation index, Page parentPage, STreeEdgeIdData *edgeId, int splitCharPos)
{
    STreeEdgeInsideData    *oldEdge;
    Buffer                  newNodeBuffer;
    Page                    newNodePage;
    BlockNumber             newNodeBlkno;
    BlockNumber             originalDestNode;
    char                   *labelStart;
    char                   *labelPtr;
    int                     totalLabelBytes;
    int                     prefixBytes;
    int                     suffixBytes;
    int                     charCount;
    pg_wchar                firstCharOfSuffix;

    Assert(parentPage != NULL);
    Assert(edgeId != NULL);
    Assert(splitCharPos > 0);

    /* Get the original edge data */
    oldEdge = streeGetEdgeData(parentPage, edgeId);
    originalDestNode = oldEdge->destinationNode;
    labelStart = oldEdge->label;
    totalLabelBytes = oldEdge->labelLength;

    /*
     * Find the byte position corresponding to splitCharPos characters.
     * We need to walk through UTF-8 characters.
     */
    labelPtr = labelStart;
    charCount = 0;
    while (charCount < splitCharPos && (labelPtr - labelStart) < totalLabelBytes)
    {
        labelPtr += pg_mblen(labelPtr);
        charCount++;
    }

    prefixBytes = labelPtr - labelStart;
    suffixBytes = totalLabelBytes - prefixBytes;

    /* Get the first character of the suffix (for the new edge) */
    if (suffixBytes > 0)
    {
        pg_mb2wchar_with_len((const unsigned char *) labelPtr, &firstCharOfSuffix, 
                             pg_mblen(labelPtr));
    }
    else
    {
        /* Edge case: split at the end - shouldn't happen normally */
        elog(WARNING, "splitEdge: split position at end of edge");
        return InvalidBlockNumber;
    }

    /*
     * Allocate a new page for the internal node.
     * STreeGetNewBuffer returns a locked buffer.
     */
    newNodeBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(newNodeBuffer))
    {
        elog(WARNING, "splitEdge: could not allocate new buffer");
        return InvalidBlockNumber;
    }

    newNodeBlkno = BufferGetBlockNumber(newNodeBuffer);
    /* Buffer is already exclusively locked by STreeGetNewBuffer */
    newNodePage = BufferGetPage(newNodeBuffer);

    START_CRIT_SECTION();

    /* Initialize the new internal node page */
    STreeInitPage(newNodePage, STREE_EDGE_NODE_PAGE);

    /* Set up the opaque data for new node */
    {
        STreeNodePageOpaque newOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(newNodePage);
        STreeNodePageOpaque parentOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(parentPage);
        
        newOpaque->parentNode = BufferGetBlockNumber(
            /* We need the parent's block number - caller should provide this */
            /* For now, assume parent page's block is tracked elsewhere */
            InvalidBlockNumber  /* TODO: pass parentBlkno as parameter */
        );
        newOpaque->suffixLink = InvalidBlockNumber;  /* Caller will set this */
        newOpaque->firstNode = InvalidBlockNumber;
        newOpaque->prevSiblingNode = InvalidBlockNumber;
        newOpaque->nextSiblingNode = InvalidBlockNumber;
        newOpaque->itemPointersStart = InvalidBlockNumber;
        newOpaque->streePageId = STREE_PAGE_ID;
    }

    /*
     * Create an edge from the new internal node to the original destination.
     * This edge has the suffix of the original label.
     */
    {
        STreeEdgeIdData *suffixEdge;
        
        suffixEdge = insertEdgeSorted(newNodePage, 
                                      firstCharOfSuffix,
                                      labelPtr,         /* suffix label bytes */
                                      suffixBytes,
                                      originalDestNode);
        
        if (suffixEdge == NULL)
        {
            /* This shouldn't happen on a fresh page */
            END_CRIT_SECTION();
            UnlockReleaseBuffer(newNodeBuffer);
            elog(WARNING, "splitEdge: could not insert suffix edge");
            return InvalidBlockNumber;
        }
    }

    /*
     * Update the original edge in the parent page:
     * - Keep the same firstChar
     * - Shorten the label to just the prefix
     * - Point to the new internal node instead of original destination
     */
    {
        /* Update the edge inside data */
        oldEdge->destinationNode = newNodeBlkno;
        oldEdge->labelLength = prefixBytes;
        /* Note: label bytes stay in place, we just use fewer of them */
        
        /* Update the edge ID */
        edgeId->realEdgeDataLength = SizeOfSTreeEdgeInsideData + prefixBytes;
    }

    MarkBufferDirty(newNodeBuffer);
    /* Parent buffer is marked dirty by caller (splitEdgeWithBuffer) */

    END_CRIT_SECTION();

    UnlockReleaseBuffer(newNodeBuffer);

    return newNodeBlkno;
}

/*
 * splitEdgeWithBuffer - Version that takes parent buffer for proper dirty marking.
 */
BlockNumber
splitEdgeWithBuffer(Relation index, Buffer parentBuffer, 
                    STreeEdgeIdData *edgeId, int splitCharPos,
                    BlockNumber *newNodeBlknoOut)
{
    Page                    parentPage;
    STreeEdgeInsideData    *oldEdge;
    Buffer                  newNodeBuffer;
    Page                    newNodePage;
    BlockNumber             newNodeBlkno;
    BlockNumber             parentBlkno;
    BlockNumber             originalDestNode;
    char                   *labelStart;
    char                   *labelPtr;
    int                     totalLabelBytes;
    int                     prefixBytes;
    int                     suffixBytes;
    int                     charCount;
    pg_wchar                firstCharOfSuffix;

    Assert(BufferIsValid(parentBuffer));
    Assert(edgeId != NULL);
    Assert(splitCharPos > 0);

    parentPage = BufferGetPage(parentBuffer);
    parentBlkno = BufferGetBlockNumber(parentBuffer);

    /* Get the original edge data */
    oldEdge = streeGetEdgeData(parentPage, edgeId);
    originalDestNode = oldEdge->destinationNode;
    labelStart = oldEdge->label;
    totalLabelBytes = oldEdge->labelLength;

    /*
     * Find the byte position corresponding to splitCharPos characters.
     */
    labelPtr = labelStart;
    charCount = 0;
    while (charCount < splitCharPos && (labelPtr - labelStart) < totalLabelBytes)
    {
        labelPtr += pg_mblen(labelPtr);
        charCount++;
    }

    prefixBytes = labelPtr - labelStart;
    suffixBytes = totalLabelBytes - prefixBytes;

    if (suffixBytes <= 0)
    {
        elog(WARNING, "splitEdge: split position at or past end of edge");
        return InvalidBlockNumber;
    }

    /* Get the first character of the suffix */
    pg_mb2wchar_with_len((const unsigned char *) labelPtr, &firstCharOfSuffix, 
                         pg_mblen(labelPtr));

    /*
     * Allocate a new page for the internal node.
     * STreeGetNewBuffer returns a locked buffer.
     */
    newNodeBuffer = STreeGetNewBuffer(index);
    if (!BufferIsValid(newNodeBuffer))
    {
        elog(WARNING, "splitEdge: could not allocate new buffer");
        return InvalidBlockNumber;
    }

    newNodeBlkno = BufferGetBlockNumber(newNodeBuffer);
    /* Buffer is already exclusively locked by STreeGetNewBuffer */
    newNodePage = BufferGetPage(newNodeBuffer);

    START_CRIT_SECTION();

    /* Initialize the new internal node page */
    STreeInitPage(newNodePage, STREE_EDGE_NODE_PAGE);

    /* Set up the opaque data for new node */
    {
        STreeNodePageOpaque newOpaque = (STreeNodePageOpaque) PageGetSpecialPointer(newNodePage);
        
        newOpaque->parentNode = parentBlkno;
        newOpaque->suffixLink = InvalidBlockNumber;  /* Caller sets this later */
        newOpaque->firstNode = InvalidBlockNumber;
        newOpaque->prevSiblingNode = InvalidBlockNumber;
        newOpaque->nextSiblingNode = InvalidBlockNumber;
        newOpaque->itemPointersStart = InvalidBlockNumber;
        newOpaque->streePageId = STREE_PAGE_ID;
        newOpaque->flags = STREE_EDGE_NODE_PAGE;
    }

    /*
     * Create edge from new internal node to original destination (suffix edge).
     */
    {
        STreeEdgeIdData *suffixEdge;
        
        suffixEdge = insertEdgeSorted(newNodePage, 
                                      firstCharOfSuffix,
                                      labelPtr,
                                      suffixBytes,
                                      originalDestNode);
        
        if (suffixEdge == NULL)
        {
            END_CRIT_SECTION();
            UnlockReleaseBuffer(newNodeBuffer);
            elog(WARNING, "splitEdge: could not insert suffix edge on new page");
            return InvalidBlockNumber;
        }
    }

    /*
     * Update the original edge in parent page:
     * - Shorten label to prefix only
     * - Point to new internal node
     */
    oldEdge->destinationNode = newNodeBlkno;
    oldEdge->labelLength = prefixBytes;
    edgeId->realEdgeDataLength = SizeOfSTreeEdgeInsideData + prefixBytes;

    MarkBufferDirty(parentBuffer);
    MarkBufferDirty(newNodeBuffer);

    END_CRIT_SECTION();

    UnlockReleaseBuffer(newNodeBuffer);

    if (newNodeBlknoOut)
        *newNodeBlknoOut = newNodeBlkno;

    return newNodeBlkno;
}
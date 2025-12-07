/**---------------------------------------------------------------
 * suffixtree/stree_private.h
 *        Private declarations for Suffix Tree access method.
 * 
 *----------------------------------------------------------------
 */
#ifndef STREE_PRIVATE_H
#define STREE_PRIVATE_H

#include "access/itup.h"
#include "access/amapi.h"
#include "mb/pg_wchar.h"

/* Tunning points */
#define STREE_BSEARCH_THRESHOLD 16

/* Suffix Tree state information */
typedef struct STreeState
{
    Relation index; // index we're working with
    /* state */
    //SuffixTree *tree;

    TransactionId redirectXid;	// debug purpose
    bool isBuild; // true if building the index
} STreeState;

typedef struct STreeActiveNode
{
    BlockNumber blockNumber; // block number of the node
    Buffer buffer;           // buffer of the node
    Page page;               // page pointer of the node
} STreeActiveNode;

typedef struct STreeBuildState {
    STreeState indexState;		/* Suffix Tree's working state */
    int64		indexedTuples;		/* total number of tuples indexed */
    MemoryContext tmpMemCtx;		/* per-tuple temporary context */
    STreeActiveNode activeNode;   /* currently active node during insertion */
} STreeBuildState;

#define STREE_MAGIC_NUMBER (0xC0FFEE42) // Used to identify memory corruption detection

//Index of the column that is going to be indexed
#define streeIndexedColumn 0

/* Page numbers of fixed-location pages */
#define STREE_METAPAGE_BLK	  (0)	/* metapage */
#define STREE_ROOT_BLK		  (1)	/* root for single byte entries */
// #define STREE_NULL_BLK		  (2)	/* root for null-value entries */
#define STREE_LAST_FIXED_BLK  STREE_ROOT_BLK

#define STreeBlockIsRoot(blkno) \
    ((blkno) == STREE_ROOT_BLK)
#define STreeBlockIsFixed(blkno) \
    ((BlockNumber) (blkno) <= (BlockNumber) STREE_LAST_FIXED_BLK)

/* Flag bits in page special space */
#define STREE_META          (1<<0)
#define STREE_ROOT		    (1<<1)
#define STREE_EDGE_NODE_PAGE	(1<<2)
#define STREE_DATA_NODE_PAGE	(1<<3)

#define STreeBlockIsRoot(blkno) \
    ((blkno) == STREE_ROOT_BLK)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 */
#define STREE_PAGE_ID		0xCDEE

#define STREE_CACHED_PAGES 16

typedef struct STreeLastUsedPage
{
    BlockNumber lastUsedPage;   /* last used page */
} STreeLastUsedPage;

typedef struct STreeMetaPageData
{
    uint32_t magicNumber;	/* for identity cross-check */
    uint64_t numberOfIndexedValues;
    STreeLastUsedPage cachedPages[STREE_CACHED_PAGES];
    // Other metadata fields can be added here (cache size, version, etc.)
} STreeMetaPageData;

#define STreePageGetMetaStart(p) \
    ((STreeMetaPageData *) PageGetContents(p))

// typedef struct STreeRootNodeEntry
// {
//     BlockNumber childBlockNumber;
//     pg_wchar  edgeLabel; // edge label (substring)
// } STreeRootNodeEntry;

// typedef struct STreeRootNodeInnerData {
//     uint32      numberOfEntries;   /* number of entries in the root node */
//     STreeRootNodeEntry entries[FLEXIBLE_ARRAY_MEMBER]; /* array of root node entries */
// } STreeRootNodeInnerData;

// typedef STreeRootNodeInnerData *STreeRootNodeInner;


typedef struct STreeNodePageOpaqueData
{
    BlockNumber parentNode;       /* block number of the parent node */
    BlockNumber prevSiblingNode; /* block number of the sibling node */
    BlockNumber nextSiblingNode; /* block number of the sibling node */
    BlockNumber itemPointersStart;     /* block number of item pointers page */
    BlockNumber firstNode;        /* block number of the first child node (the first node stores the edges and has STREE_EDGE_NODE_PAGE flag) */
    BlockNumber suffixLink;       /* block number of the suffix link node */
    uint16      streePageId;    /* for identification of Suffix Tree indexes */
    uint16		flags;			   /* see bit definitions above */
} STreeNodePageOpaqueData;

typedef STreeNodePageOpaqueData *STreeNodePageOpaque;

typedef struct STreeEdgeIdArrayHeader
{
    uint32      numberOfEdges;       /* number of edges on this page */
    /* Followed by STreeEdgeIdData[numEdges] */
} STreeEdgeIdArrayHeader;

#define STreePageGetEdgeIdHeader(page) \
    ((STreeEdgeIdArrayHeader *) PageGetContents(page))

#define STreePageGetNumEdges(page) \
    (STreePageGetEdgeIdHeader(page)->numberOfEdges)

#define STreePageGetEdgeIds(page) \
    ((STreeEdgeIdData *) (((char *) PageGetContents(page)) + sizeof(STreeEdgeIdArrayHeader)))

/*
 * Calculate size needed for edge ID array
 */
#define STreeEdgeIdArraySize(numEdges) \
    (sizeof(STreeEdgeIdArrayHeader) + ((numEdges) * sizeof(STreeEdgeIdData)))

typedef struct STreeEdgeIdData
{
    //pg_wchar firstChar; // for utf8 support?
    pg_wchar firstChar; // first character of the edge label
    uint16  realEdgeOffset: 16; // offset of the edge in the page
    uint16  realEdgeDataLength: 16; // length of the edge data
} STreeEdgeIdData;

typedef STreeEdgeIdData *STreeEdgeId;

// #define EDGE_HIGH_CHAR      ((OffsetNumber) 1)
// #define EDGE_FIRST_ID_DATA  ((OffsetNumber) 2)

/*
 * Macros to access EdgeId fields
 */
#define EdgeIdGetFirstChar(edgeId)  ((edgeId)->firstChar)
#define EdgeIdGetOffset(edgeId)     ((edgeId)->realEdgeOffset)
#define EdgeIdGetStringByteLength(edgeId)     ((edgeId)->realEdgeDataLength)


typedef struct STreeEdgeInsideData
{
    BlockNumber destinationNode;    /* 4 bytes - destination node number */
    uint16 labelLength;            /* BYTE count, not character count */
    char label[FLEXIBLE_ARRAY_MEMBER];  /* raw UTF-8 bytes */
} STreeEdgeInsideData;

typedef STreeEdgeInsideData *STreeEdgeInside;

#define SizeOfSTreeEdgeInsideData  offsetof(STreeEdgeInsideData, label)



typedef struct STreeNodeTupleDataEntries {
    unsigned int numberOfEntries: 32;   /* number of data items  */
    /*      * Followed immediately by: */
    /* - array of StreeNodeTupleData[numberOfEntries] */
} STreeNodeTupleDataEntries;
typedef STreeNodeTupleDataEntries *STreeNodeTupleDataEntriesPtr;

#define STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE    MAXALIGN(sizeof(STreeNodeTupleDataEntries))
#define STREE_NODE_TUPLE_DATA_ENTRIES_SIZE		(sizeof(STreeNodeTupleDataEntries) + \
                                         (sizeof(STreeNodeTupleData) * (numberOfEntries)))
#define STREE_NODE_TUPLE_DATA_ENTRIES_PTR(x)		((x)->numberOfEntries > 0 ? ((char *) (x)) + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE : NULL)

typedef IndexTupleData STreeNodeTupleData;

typedef STreeNodeTupleData *STreeNodeTuple;

#define STREE_NODE_TUPLE_HEADER_SIZE		MAXALIGN(sizeof(STreeNodeTupleData))
#define STREE_NODE_DATA_PTR(x)		(((char *) (x)) + STREE_NODE_TUPLE_HEADER_SIZE)
#define STREE_NODE_TUPLE_DATUM(x, s)		((s)->byval ? \
                                         *(Datum *) STREE_NODE_DATA_PTR(x) : \
                                         PointerGetDatum(STREE_NODE_DATA_PTR(x)))


// Inline function declarations
/*
 * Helper: Get byte length of a pg_wchar when encoded in database encoding.
 */
static inline pg_wchar
streeGetNextChar(const char **strptr, const char *strend)
{
    const char *s = *strptr;
    int         mblen;
    pg_wchar    result;

    if (s >= strend)
        return 0;  /* End of string */

    /* Get byte length of this multibyte character */
    mblen = pg_mblen(s);
    
    /* Safety check - don't go past end */
    if (s + mblen > strend)
        mblen = strend - s;

    /* Convert multibyte char to wide character (Unicode codepoint) */
    pg_mb2wchar_with_len((const unsigned char *) s, &result, mblen);

    /* Advance pointer by the number of bytes consumed */
    *strptr += mblen;

    return result;
}

/*
 * Helper: Get byte length of a pg_wchar when encoded in database encoding.
 */
static inline int
streeWcharMblen(pg_wchar wc)
{
    unsigned char buf[MAX_MULTIBYTE_CHAR_LEN + 1];
    
    return pg_wchar2mb_with_len(&wc, (char *) buf, 1);
}

/*
 * Helper: Get EdgeInsideData pointer from an edge ID.
 */
static inline STreeEdgeInsideData *
streeGetEdgeData(Page page, STreeEdgeIdData *edgeId)
{
    return (STreeEdgeInsideData *) (((char *) page) + edgeId->realEdgeOffset);
}

/* Helper: Get the remaining label (after firstChar) */
static inline char *
STreeGetEdgeLabel(STreeEdgeInsideData *edge)
{
    return ((char *) edge) + SizeOfSTreeEdgeInsideData;
}

/* Get destination node */
static inline BlockNumber
streeGetEdgeDestination(Page page, STreeEdgeIdData *edgeId)
{
    STreeEdgeInsideData *edge = streeGetEdgeData(page, edgeId);
    return edge->destinationNode;
}

/*
 * 
 * Helper: Get the length of an edge label in characters.
 *
 * Parameters:
 *   edgeData - Pointer to the edge inside data
 *
 * Returns:
 *   Number of characters in the label.
 */
static inline int
getEdgeLabelCharLength(STreeEdgeInsideData *edgeData)
{
    if (edgeData == NULL || edgeData->labelLength == 0)
        return 0;

    return pg_mbstrlen_with_len(edgeData->label, edgeData->labelLength);
}

/* Helper to get character at position within edge label */
static inline pg_wchar
getEdgeLabelCharAt(STreeEdgeInsideData *edgeData, int charPos)
{
    const char *labelPtr;
    const char *labelEnd;
    int         i;
    pg_wchar    result = 0;

    if (edgeData == NULL || charPos < 0)
        return 0;

    labelPtr = edgeData->label;
    labelEnd = edgeData->label + edgeData->labelLength;

    for (i = 0; i < charPos && labelPtr < labelEnd; i++)
    {
        labelPtr += pg_mblen(labelPtr);
    }

    if (labelPtr >= labelEnd)
        return 0;

    pg_mb2wchar_with_len((const unsigned char *) labelPtr,
                         &result,
                         pg_mblen(labelPtr));

    return result;
}

/*
 * Helper: getByteOffsetForCharPos - Get byte pointer for a character position.
 */
static inline const char *
getByteOffsetForCharPos(const char *strValue, const char *strEnd, int charPos)
{
    const char *ptr = strValue;
    int         i;

    for (i = 0; i < charPos && ptr < strEnd; i++)
    {
        ptr += pg_mblen(ptr);
    }

    return ptr;
}

/*
 * Helper: getCharAtPosition - Get the Unicode codepoint at a character position.
 */
static inline pg_wchar
getCharAtPosition(const char *strValue, const char *strEnd, int charPos)
{
    const char *ptr = strValue;
    int         i;
    pg_wchar    result = 0;

    for (i = 0; i < charPos && ptr < strEnd; i++)
    {
        ptr += pg_mblen(ptr);
    }

    if (ptr >= strEnd)
        return 0;

    pg_mb2wchar_with_len((const unsigned char *) ptr, &result, pg_mblen(ptr));
    return result;
}

// extern bool streedoinsert(Relation index, SpGistState *state,
// 						ItemPointer heapPtr, Datum *datums, bool *isnulls);
extern void STreeInitPage(Page page, uint16 f);
extern void STreeInitMetapage(Page page);
extern Buffer STreeGetNewBuffer(Relation index);
extern void initSTreeState(STreeState *state, Relation index);
extern bool streeinserttuple(Relation index, STreeBuildState *state,
                 ItemPointer tid, Datum *values, bool *isnull);

/* Edge operations */
extern STreeEdgeIdData *lookupEdgeByFirstChar(Page page, pg_wchar firstChar);
extern STreeEdgeIdData *insertEdgeSorted(Page page, pg_wchar firstChar, 
                                         const char *labelData, uint16 labelLen,
                                         BlockNumber destBlock);
extern BlockNumber splitEdgeWithBuffer(Relation index, Buffer parentBuffer, 
                                       STreeEdgeIdData *edgeId, int splitCharPos,
                                       BlockNumber *newNodeBlknoOut);
extern bool walkDown(Relation index, STreeActiveNode *activeNode, int *activeEdgeCharIdx,
              int *activeLength, const char *strValue, const char *strEnd, Buffer rootBuffer);

/* Data page operations for storing heap TIDs */
extern bool streeAddHeapTid(Relation index, Buffer leafBuffer, 
                            STreeEdgeIdData *edgeId, ItemPointer tid);
extern Buffer streeAllocateDataPage(Relation index, Buffer leafBuffer, 
                                    STreeEdgeIdData *edgeId);
extern Buffer streeAllocateOverflowDataPage(Relation index, Buffer leafBuffer,
                                            STreeEdgeIdData *edgeId, 
                                            BlockNumber currentBlkno);
extern int streeGetDataPageTids(Relation index, BlockNumber dataBlkno,
                                void (*callback)(ItemPointer tid, void *arg), 
                                void *callbackArg);
#endif   /* STREE_PRIVATE_H */
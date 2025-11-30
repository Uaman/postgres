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
#define STreeKeyColumn 0

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

typedef struct STreeRootNodeEntry
{
    BlockNumber childBlockNumber;
    unsigned char  edgeLabel; // edge label (substring)
} STreeRootNodeEntry;

typedef struct STreeRootNodeInnerData {
    uint32      numberOfEntries;   /* number of entries in the root node */
    STreeRootNodeEntry entries[FLEXIBLE_ARRAY_MEMBER]; /* array of root node entries */
} STreeRootNodeInnerData;

typedef STreeRootNodeInnerData *STreeRootNodeInner;


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

typedef struct STreeEdgeIdData
{
    //pg_wchar firstChar; // for utf8 support?
    unsigned char firstChar; // first character of the edge label
    unsigned short  edgeOffset: 16; // offset of the edge label in the page
    unsigned short  edgeLength: 16; // length of the edge label
    
    // // pg_wchar    *edgeLabel; // something for utf8 support
} STreeEdgeIdData;

typedef STreeEdgeIdData *STreeEdgeId;

#define SizeOfEdgeIdData (sizeof(STreeEdgeIdData))

/*
 * Macros to access EdgeId fields
 */
#define EdgeIdGetFirstChar(edgeId)  ((edgeId)->first_char)
#define EdgeIdGetOffset(edgeId)     ((edgeId)->offset)
#define EdgeIdGetLength(edgeId)     ((edgeId)->length)
#define EdgeIdGetFlags(edgeId)      ((edgeId)->flags)
#define EdgeIdIsNormal(edgeId)      ((edgeId)->flags == EDGE_NORMAL)


typedef struct EdgeInsideData
{
    BlockNumber destinationNode;    /* 4 bytes - destination node number */
    uint16 labelLength;            /* 2 bytes - length of label string */
    //uint16 flags;               /* 2 bytes - edge attributes */
    /*
     * Followed immediately by:
     * - label string (labelLength bytes) unsigned char*
     */
} EdgeInsideData;

typedef EdgeInsideData *EdgeData;

#define EdgeDataGetDestination(edge)    ((edge)->destination_node)
#define EdgeDataGetLabelLength(edge)    ((edge)->label_length)

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


// extern bool streedoinsert(Relation index, SpGistState *state,
// 						ItemPointer heapPtr, Datum *datums, bool *isnulls);
extern void STreeInitPage(Page page, uint16 f);
extern void STreeInitMetapage(Page page);
extern Buffer STreeGetNewBuffer(Relation index);
extern void initSTreeState(STreeState *state, Relation index);
extern bool streeinserttuple(Relation index, STreeBuildState *state,
                 ItemPointer tid, Datum *values, bool *isnull);
#endif   /* STREE_PRIVATE_H */
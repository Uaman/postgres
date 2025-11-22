/**---------------------------------------------------------------
 * suffixtree/stree_private.h
 *        Private declarations for Suffix Tree access method.
 * 
 *----------------------------------------------------------------
 */
#ifndef STREE_PRIVATE_H
#define STREE_PRIVATE_H

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
} StreeState;

typedef struct STreeBuildState 
{
    MemoryContext tmpCtx; // per-tuple temporary context
} STreeBuildState;

#define STREE_MAGIC_NUMBER (0xC0FFEE42) // Used to identify memory corruption detection

//Index of the column that is going to be indexed
#define streeKeyColumn 0

/* Page numbers of fixed-location pages */
#define STREE_METAPAGE_BLK	  (0)	/* metapage */
#define STREE_ROOT_BLK		  (1)	/* root for single byte entries */
// #define STREE_NULL_BLK		  (2)	/* root for null-value entries */
#define STREE_LAST_FIXED_BLK  STREE_ROOT_BLK

#define StreeBlockIsRoot(blkno) \
    ((blkno) == STREE_ROOT_BLK)
#define StreeBlockIsFixed(blkno) \
    ((BlockNumber) (blkno) <= (BlockNumber) STREE_LAST_FIXED_BLK)

/* Flag bits in page special space */
#define STREE_META          (1<<0)
#define STREE_ROOT		    (1<<1)
#define STREE_EDGE_NODE_PAGE	(1<<2)
#define STREE_DATA_NODE_PAGE	(1<<3)

#define StreeBlockIsRoot(blkno) \
    ((blkno) == STREE_ROOT_BLK)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 */
#define STREE_PAGE_ID		0xCDEE

typedef struct STreeMetaPageData
{
    uint32_t magicNumber;	/* for identity cross-check */
    uint32_t numberOfIndexedValues;
    // Other metadata fields can be added here (cache size, version, etc.)
} STreeMetaPageData;

typedef struct STreeRootNodeEntry
{
    BlockNumber childBlockNumber;
    unsigned char  edgeLabel; // edge label (substring)
} STreeRootNodeEntry;

typedef struct StreeRootNodeInnerData {
    uint32      numberOfEntries;   /* number of entries in the root node */
    STreeRootNodeEntry entries[FLEXIBLE_ARRAY_MEMBER]; /* array of root node entries */
} StreeRootNodeInnerData;

typedef StreeRootNodeInnerData *StreeRootNodeInner;


typedef struct StreeNodePageOpaqueData
{
    BlockNumber parentNode;       /* block number of the parent node */
    BlockNumber prevSiblingNode; /* block number of the sibling node */
    BlockNumber nextSiblingNode; /* block number of the sibling node */
    BlockNumber itemPointersStart;     /* block number of item pointers page */
    BlockNumber firstNode;        /* block number of the first child node (the first node stores the edges and has STREE_EDGE_NODE_PAGE flag) */
    BlockNumber suffixLink;       /* block number of the suffix link node */
    uint16      streePageId;    /* for identification of Suffix Tree indexes */
    uint16		flags;			   /* see bit definitions above */
} StreeNodePageOpaqueData;

typedef struct StreeEdgeIdData
{
    //pg_wchar firstChar; // for utf8 support?
    unsigned char firstChar; // first character of the edge label
    unsigned short  edgeOffset: 16; // offset of the edge label in the page
    unsigned short  edgeLength: 16; // length of the edge label
    
    // // pg_wchar    *edgeLabel; // something for utf8 support
} StreeEdgeIdData;

typedef StreeEdgeIdData *StreeEdgeId;

#define SizeOfEdgeIdData (sizeof(StreeEdgeIdData))

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

typedef struct StreeNodeTupleDataEntries {
    unsigned int numberOfEntries: 32;   /* number of data items  */
    /*      * Followed immediately by: */
    /* - array of StreeNodeTupleData[numberOfEntries] */
} StreeNodeTupleDataEntries;

typedef StreeNodeTupleDataEntries *StreeNodeTupleDataEntriesPtr;

#define STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE    MAXALIGN(sizeof(StreeNodeTupleDataEntries))
#define STREE_NODE_TUPLE_DATA_ENTRIES_SIZE		(sizeof(StreeNodeTupleDataEntries) + \
                                         (sizeof(StreeNodeTupleData) * (numberOfEntries)))
#define STREE_NODE_TUPLE_DATA_ENTRIES_PTR(x)		((x)->numberOfEntries > 0 ? ((char *) (x)) + STREE_NODE_TUPLE_DATA_ENTRIES_HEADER_SIZE : NULL)




typedef IndexTupleData StreeNodeTupleData;

typedef StreeNodeTupleData *StreeNodeTuple;

#define STREE_NODE_TUPLE_HEADER_SIZE		MAXALIGN(sizeof(StreeNodeTupleData))
#define STREE_NODE_DATA_PTR(x)		(((char *) (x)) + STREE_NODE_TUPLE_HEADER_SIZE)
#define STREE_NODE_TUPLE_DATUM(x, s)		((s)->byval ? \
                                         *(Datum *) STREE_NODE_DATA_PTR(x) : \
                                         PointerGetDatum(STREE_NODE_DATA_PTR(x)))




extern bool streedoinsert(Relation index, SpGistState *state,
						ItemPointer heapPtr, Datum *datums, bool *isnulls);
extern Buffer StreeInitNewBuffer(Relation index);
#endif   /* STREE_PRIVATE_H */
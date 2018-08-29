/*
*   this extension is for building a custom scan node to implement the K-inCircle search 
*   algorithm mentioned in the paper 
*/

#define _GNU_SOURCE

#include "postgres.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "pgstat.h"
#include <sys/time.h>
#include <time.h> 
#include <math.h>

#include "utils/elog.h"
#include "utils/geo_decls.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"
#include "utils/datum.h"
#include "utils/index_selfuncs.h"
#include "utils/selfuncs.h"
#include "utils/pg_locale.h"

#include "libpq/pqformat.h"		/* needed for send/recv functions */
#include "libpq/libpq.h"

#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/var.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/placeholder.h"

#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"

#include "nodes/print.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/bitmapset.h"
#include "nodes/extensible.h"

#include "access/sysattr.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/gist.h"
#include "access/relscan.h"
#include "access/amapi.h"
#include "access/spgist_private.h"

#include "executor/executor.h"
#include "executor/nodeIndexscan.h"

#include "rewrite/rewriteManip.h"
#include "parser/parsetree.h"

#include "storage/dsm_impl.h"
#include "storage/off.h"
#include "storage/bufpage.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
//#ifdef PG_MODULE_MAGIC
/* >= 8.2 */
PG_MODULE_MAGIC;

//#endif

#ifndef OPTIMIZER_DEBUG
#define OPTIMIZER_DEBUG
#endif


#undef SPGISTNProc
#define SPGISTNProc 6

#define SPGIST_DISTANCE_POINT_PROC 6
// #define MY_SPGIST_POINT_DISTANCE 6
// ================================
//      For hook functions 
//=================================
static planner_hook_type prev_planner = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static join_search_hook_type prev_join_search_hook = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;
static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;


void _PG_init(void);
void _PG_fini(void);

static PlannedStmt *myplanner(Query *parse, int cursorOptions,
        ParamListInfo boundParams);

static void my_set_relpathlist(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte);

static RelOptInfo * my_join_search_hook(PlannerInfo *root, 
                            int levels_needed,
                            List *initial_rels);

static void my_set_join_pathlist_hook(PlannerInfo * root, 
                                      RelOptInfo * joinrel, 
                                      RelOptInfo * outerrel, 
                                      RelOptInfo * innerrel,
                                      JoinType jointype,
                                      JoinPathExtraData *extra);

static void my_create_upper_paths_hook (PlannerInfo *root,
                           UpperRelationKind stage,
                             RelOptInfo *input_rel,
                           RelOptInfo *output_rel);
static void my_ExecutorStart_hook(QueryDesc *queryDesc, int eflags);

//Version 9.6.4
static void my_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count);
//Version 4 
//void my_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once);
static void my_ExecutorFinish_hook();
static void my_ExecutorEnd_hook();
void _PG_init(void)
{
  // install hooks
  elog(NOTICE, "\nKNN Hook lib is loaded\n\n\n");
  
  prev_planner = planner_hook;
  planner_hook = myplanner;


  prev_set_rel_pathlist = set_rel_pathlist_hook;
  set_rel_pathlist_hook = my_set_relpathlist;

  prev_create_upper_paths_hook = create_upper_paths_hook;
  create_upper_paths_hook = my_create_upper_paths_hook;

  prev_join_search_hook = join_search_hook;
  join_search_hook = my_join_search_hook;

  prev_set_join_pathlist_hook = set_join_pathlist_hook;
  set_join_pathlist_hook = my_set_join_pathlist_hook;

  prev_ExecutorStart_hook = ExecutorStart_hook;
  ExecutorStart_hook = my_ExecutorStart_hook;

  prev_ExecutorRun_hook = ExecutorRun_hook;
  ExecutorRun_hook = my_ExecutorRun_hook;

  // prev_ExecutorFinish_hook = ExecutorFinish_hook;
  // ExecutorFinish_hook = my_ExecutorFinish_hook;

  // prev_ExecutorEnd_hook = ExecutorEnd_hook;
  // ExecutorEnd_hook = my_ExecutorEnd_hook;
}

void
_PG_fini(void)
{
  /* Uninstall hooks. */
  planner_hook = prev_planner;

  set_rel_pathlist_hook = prev_set_rel_pathlist;

  create_upper_paths_hook = prev_create_upper_paths_hook;

  join_search_hook = prev_join_search_hook;
}

//===========================================
//      other DataStructure Definitions
//===========================================

typedef struct
{
  pairingheap_node ph_node;
  HeapTuple htup;
  Datum    *orderbyvals;
  bool     *orderbynulls;
} ReorderTuple;

//-----------------

typedef struct KInCircle_data
{
  int k ; 
  IndexScan indexscanNode;
  // OpExpr* opr; // to hold the <-> operator with both arguments
  // Oid     indexid;    /* OID of index to scan */
} KInCircle_data;

//-----------------

//define a larger sttructure for customscanstate
typedef struct KInCircleState
{
  CustomScanState base;
  IndexScanState indexstate;
  //List     *indexorderbyorig;
  //Relation  iss_RelationDesc; // Index Realation Desc
  Oid     indexid;    /* OID of index to scan */
} KInCircleState;

typedef struct CacheCustomScanState
{
  CustomScanState base;
  /* define the hash table for the caching */

} CacheCustomScanState;

/*
 * During a GiST index search, we must maintain a queue of unvisited items,
 * which can be either individual heap tuples or whole index pages.  If it
 * is an ordered search, the unvisited items should be visited in distance
 * order.  Unvisited items at the same distance should be visited in
 * depth-first order, that is heap items first, then lower index pages, then
 * upper index pages; this rule avoids doing extra work during a search that
 * ends early due to LIMIT.
 *
 * To perform an ordered search, we use a pairing heap to manage the
 * distance-order queue.  In a non-ordered search (no order-by operators),
 * we use it to return heap tuples before unvisited index pages, to
 * ensure depth-first order, but all entries are otherwise considered
 * equal.
 */

/* Individual heap tuple to be visited */
typedef struct SPGISTSearchHeapItem
{
  ItemPointerData heapPtr;
  bool    recheck;    /* T if quals must be rechecked */
  bool    recheckDistances;   /* T if distances must be rechecked */
  IndexTuple  ftup;     /* data fetched back from the index, used in
                 * index-only scans */
  OffsetNumber offnum;    /* track offset in page to mark tuple as
                 * LP_DEAD */
} SPGISTSearchHeapItem;

/* Unvisited item, either index page or heap tuple */
typedef struct SPGISTSearchItem
{
  pairingheap_node phNode;
  BlockNumber blkno;      /* index page number, or InvalidBlockNumber */
  ItemPointerData ptr;    /* block and offset to scan from */
  int level;
  Point P_center; /* parent center */
  Point P_min, P_max; /* corner points of parent bounding box*/
  union
  {
    //GistNSN   parentlsn;  /* parent page's LSN, if index page */
    /* we must store parentlsn to detect whether a split occurred */
    SPGISTSearchHeapItem heap;  /* heap info, if heap tuple */
  }     data;
  double    distances[FLEXIBLE_ARRAY_MEMBER];   /* numberOfOrderBys
                             * entries */
} SPGISTSearchItem;

#define SPGISTSearchItemIsHeap(item)  ((item).blkno == InvalidBlockNumber)
// #define SPGISTSearchItemIsHeap(item)  ((item).ptr. == InvalidBlockNumber)

#define SizeOfSPGISTSearchItem(n_distances) (offsetof(SPGISTSearchItem, distances) + sizeof(double) * (n_distances))

typedef struct my_SpGistScanOpaqueData
{
  /* This is the original defenition of the opaque data*/

  SpGistState state;      /* see above */
  MemoryContext tempCxt;    /* short-lived memory context */

  /* Control flags showing whether to search nulls and/or non-nulls */
  bool    searchNulls;  /* scan matches (all) null entries */
  bool    searchNonNulls; /* scan matches (some) non-null entries */

  /* Index quals to be passed to opclass (null-related quals removed) */
  int     numberOfKeys; /* number of index qualifier conditions */
  ScanKey   keyData;    /* array of index qualifier descriptors */

  /* Stack of yet-to-be-visited pages */
  List     *scanStack;    /* List of ScanStackEntrys */

  /* These fields are only used in amgetbitmap scans: */
  TIDBitmap  *tbm;      /* bitmap being filled */
  int64   ntids;      /* number of TIDs passed to bitmap */

  /* These fields are only used in amgettuple scans: */
  bool    want_itup;    /* are we reconstructing tuples? */
  TupleDesc indexTupDesc; /* if so, tuple descriptor for them */
  int     nPtrs;      /* number of TIDs found on current page */
  int     iPtr;     /* index for scanning through same */
  ItemPointerData heapPtrs[MaxIndexTuplesPerPage];  /* TIDs from cur page */
  bool    recheck[MaxIndexTuplesPerPage]; /* their recheck flags */
  IndexTuple  indexTups[MaxIndexTuplesPerPage];   /* reconstructed tuples */

  /* --------------------------------------
   * ----  ORDER By support  
   * -------------------------------------- */
  
  MemoryContext scanCxt;    /* context for scan-lifespan data */
  Oid      *orderByTypes; /* datatypes of ORDER BY expressions */

  pairingheap *queue;     /* queue of unvisited items */
  MemoryContext queueCxt;   /* context holding the queue */
  bool    qual_ok;    /* false if qual can never be satisfied */
  bool    firstCall;    /* true until first gistgettuple call */

  /* pre-allocated workspace arrays */
  double     *distances;    /* output area for gistindex_keytest */

  /* info about killed items if any (killedItems is NULL if never used) */
  OffsetNumber *killedItems;  /* offset numbers of killed items */
  int     numKilled;    /* number of currently stored items */
  BlockNumber curBlkno;   /* current number of block */
  //GistNSN   curPageLSN;   /* pos in the WAL stream when page was read */

  /* In a non-ordered search, returnable heap items are stored here: */
  SPGISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
  OffsetNumber nPageData;   /* number of valid items in array */
  OffsetNumber curPageData; /* next item to return */

} my_SpGistScanOpaqueData;

typedef my_SpGistScanOpaqueData *my_SpGistScanOpaque;


typedef struct mySpGistScanOpaqueData
{
  /* This is the original defenition of the opaque data*/

  SpGistState state;      /* see above */
  MemoryContext tempCxt;    /* short-lived memory context */

  /* Control flags showing whether to search nulls and/or non-nulls */
  bool    searchNulls;  /* scan matches (all) null entries */
  bool    searchNonNulls; /* scan matches (some) non-null entries */

  /* Index quals to be passed to opclass (null-related quals removed) */
  int     numberOfKeys; /* number of index qualifier conditions */
  ScanKey   keyData;    /* array of index qualifier descriptors */

  /* Stack of yet-to-be-visited pages */
  List     *scanStack;    /* List of ScanStackEntrys */

  /* These fields are only used in amgetbitmap scans: */
  TIDBitmap  *tbm;      /* bitmap being filled */
  int64   ntids;      /* number of TIDs passed to bitmap */

  /* These fields are only used in amgettuple scans: */
  bool    want_itup;    /* are we reconstructing tuples? */
  TupleDesc indexTupDesc; /* if so, tuple descriptor for them */
  int     nPtrs;      /* number of TIDs found on current page */
  int     iPtr;     /* index for scanning through same */
  ItemPointerData heapPtrs[MaxIndexTuplesPerPage];  /* TIDs from cur page */
  bool    recheck[MaxIndexTuplesPerPage]; /* their recheck flags */
  IndexTuple  indexTups[MaxIndexTuplesPerPage];   /* reconstructed tuples */

  /* --------------------------------------
   * ----  ORDER By support  
   * -------------------------------------- */
  
  MemoryContext scanCxt;    /* context for scan-lifespan data */
  Oid      *orderByTypes; /* datatypes of ORDER BY expressions */

  pairingheap *queue;     /* queue of unvisited items */
  MemoryContext queueCxt;   /* context holding the queue */
  bool    qual_ok;    /* false if qual can never be satisfied */
  bool    firstCall;    /* true until first gistgettuple call */

  /* pre-allocated workspace arrays */
  double     *distances;    /* output area for gistindex_keytest */

  /* info about killed items if any (killedItems is NULL if never used) */
  OffsetNumber *killedItems;  /* offset numbers of killed items */
  int     numKilled;    /* number of currently stored items */
  BlockNumber curBlkno;   /* current number of block */
  //GistNSN   curPageLSN;   /* pos in the WAL stream when page was read */

  /* In a non-ordered search, returnable heap items are stored here: */
  // SPGISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
  // OffsetNumber nPageData;   /* number of valid items in array */
  // OffsetNumber curPageData; /* next item to return */

} mySpGistScanOpaqueData;

typedef mySpGistScanOpaqueData *mySpGistScanOpaque;


typedef struct {
  CustomPath    custompath;

  JoinType  jointype;

  Path     *outerjoinpath;  /* path for the outer side of the join */
  Path     *innerjoinpath;  /* path for the inner side of the join */

  List     *joinrestrictinfo;   /* RestrictInfos to apply to join */

}KInCircleJoinPath;

//===========================================
//      other functions definitions
//===========================================
PlannerInfo * my_set_subquery_pathlist(PlannerInfo *root,RelOptInfo *rel, Index rti, RangeTblEntry *rte);
void my_set_Customsubquery_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
void my_set_relpathlist2(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte);
void my_set_relpathlist3(PlannerInfo *root,RelOptInfo *rel, Index rti, RangeTblEntry *rte);
void my_set_relpathlist4(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte);
void my_set_relpathlist5(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte);
void my_set_relpathlist6(PlannerInfo *root,RelOptInfo *rel,Index rti,RangeTblEntry *rte);
// debug functions
void my_debug_print_rel(PlannerInfo *root, RelOptInfo *rel);
static void my_print_path(PlannerInfo *root, Path *path, int indent);
static void my_print_restrictclauses(PlannerInfo *root, List *clauses);
static void my_print_relids(PlannerInfo *root, Relids relids);

//helper functions for re-planning 
void my_recurse_push_qual(Node *setOp, Query *topquery, RangeTblEntry *rte, Index rti, Node *qual);
void my_subquery_push_qual(Query *subquery, RangeTblEntry *rte, Index rti, Node *qual);
void my_remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel);
// static void my_get_restriction_qual_cost(PlannerInfo *root, RelOptInfo *baserel, ParamPathInfo *param_info, QualCost *qpqual_cost);
//CustomPath * create_KInCircle_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer, int parallel_workers, OpExpr* KNN_op , int k , List *pathkeys ,Path *subpath);
CustomPath * create_KInCircle_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer, OpExpr* KNN_op , int k  ,Path *subpath);
CustomPath * create_basicCustomScan_path(PlannerInfo *root, RelOptInfo *rel , Path * child_path, OpExpr* KNN_op, int k);                                  
// CustomPath * create_cacheCustomScan_path(PlannerInfo *root, RelOptInfo *rel , List * child_path);                                  
CustomPath * replace_cacheCustomScan_path(PlannerInfo *root, RelOptInfo *rel , Path * currPath, List * child_path);
CustomPath * create_materialCustom_path(Path* currentPath , Path* childPath);
CustomPath * create_materialCustom_path2(Path* currentPath , Path* childPath);
CustomPath * create_materialGenericCustom_path(Path* currentPath , Path* childPath);
CustomPath * create_CacheGenericCustom_path(Path* currentPath , Path* childPath);
// KInCircleJoinPath * create_kInCircle_Join_path(PlannerInfo *root, JoinPath * currentPath);
CustomPath * create_kInCircle_Join_path(PlannerInfo *root, JoinPath * currentPath);
CustomPath* create_kInCircle_Join_path2(PlannerInfo *root, JoinPath * currentPath, Path * innerPath);

static Plan * Plan_KInCirclePath(PlannerInfo *root,RelOptInfo *rel,struct CustomPath *best_path,List *tlist,List *clauses,List *custom_plans);
static Plan * Plan_BasicCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_CacheCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_CacheCustomPath2(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_MaterialCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_MaterialGenericCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_CacheGenericCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);
static Plan * Plan_KInCircleJoinCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);

Node *create_KInCircleScan_state(CustomScan *cscan);
Node *create_BasicCustomScan_state(CustomScan *cscan);
Node *create_CacheCustomScan_state(CustomScan *cscan);
Node *create_CacheCustomScan_state2(CustomScan *cscan);
Node *create_MaterialCustomScan_state(CustomScan *cscan);
Node *create_MaterialGenericCustomScan_state(CustomScan *cscan);
Node *create_CacheGenericCustomScan_state(CustomScan *cscan);
Node *create_KInCircleJoinCustomScan_state(CustomScan *cscan);

void Begin_KInCircleScan (CustomScanState *node, EState *estate, int eflags);
void Begin_BasicCustomScan (CustomScanState *node, EState *estate, int eflags);
void Begin_CacheCustomScan (CustomScanState *node, EState *estate, int eflags);
void Begin_MaterialCustomScan2 (CustomScanState *node, EState *estate, int eflags);
void Begin_MaterialGenericCustomScan (CustomScanState *node, EState *estate, int eflags);
void Begin_CacheGenericCustomScan (CustomScanState *node, EState *estate, int eflags);
void Begin_KInCircleJoinCustomScan (CustomScanState *node, EState *estate, int eflags);

TupleTableSlot *  Exec_KInCircleScan (CustomScanState *node);
TupleTableSlot *  Exec_BasicCustomScan (CustomScanState *node);
TupleTableSlot *  Exec_CacheCustomScan (CustomScanState *node);
TupleTableSlot *  Exec_MaterialCustomScan (CustomScanState *node);
TupleTableSlot *  Exec_MaterialGenericCustomScan (CustomScanState *node);
TupleTableSlot *  Exec_CacheGenericCustomScan (CustomScanState *node);
TupleTableSlot *  Exec_KInCircleJoinCustomScan (CustomScanState *node);

void End_KInCircleScan (CustomScanState *node);
void End_BasicCustomScan (CustomScanState *node);
void End_CacheCustomScan (CustomScanState *node);
void End_MaterialCustomScan (CustomScanState *node);
void End_MaterialGenericCustomScan (CustomScanState *node);
void End_CacheGenericCustomScan (CustomScanState *node);
void End_KInCircleJoinCustomScan (CustomScanState *node);

void ReScan_KInCircleScan (CustomScanState *node);
void ReScan_BasicCustomScan (CustomScanState *node);
void ReScan_CacheCustomScan (CustomScanState *node);
void ReScan_MaterialCustomScan (CustomScanState *node);
void ReScan_MaterialGenericCustomScan (CustomScanState *node);
void ReScan_CacheGenericCustomScan (CustomScanState *node);
void ReScan_KInCircleJoinCustomScan (CustomScanState *node);



static bool BasicCustomRecheck(CustomScanState *node, TupleTableSlot *slot);
static bool KInCircle_Recheck(CustomScanState *node, TupleTableSlot *slot);
static bool CacheCustomRecheck(CustomScanState *node, TupleTableSlot *slot);
static bool MaterialCustomRecheck(CustomScanState *node, TupleTableSlot *slot);
static bool MaterialGenericCustomRecheck(CustomScanState *node, TupleTableSlot *slot);
static bool CacheGenericCustomRecheck(CustomScanState *node, TupleTableSlot *slot);

static TupleTableSlot * KInCircle_Next(CustomScanState *node);
static TupleTableSlot * BasicCustomNext(CustomScanState *node);
static TupleTableSlot * CacheCustomNext(CustomScanState *node);
static TupleTableSlot * MaterialCustomNext(CustomScanState *node);
static TupleTableSlot * MaterialGenericCustomNext(CustomScanState *node);
static TupleTableSlot * CacheGenericCustomNext(CustomScanState *node);

static void Explain_CacheCustomScan(CustomScanState *node, List *ancestors, ExplainState *es);
static void Explain_MaterialGenericCustomScan(CustomScanState *node, List *ancestors, ExplainState *es);
static void Explain_CacheGenericCustomScan(CustomScanState *node, List *ancestors, ExplainState *es);

Path * walkPath( Path * pathRoot );
void walkPlan( Plan * planRoot, List ** custom_plans );
void walkPlanState( PlanState * planRoot, List ** custom_plans );

void cost_KInCircleScan(CustomPath *path, PlannerInfo *root, RelOptInfo *baserel, ParamPathInfo *param_info , int k);
static CustomScan * make_customScan(List *tlist, List *qual, Index scanrelid, List * custom_plans, List * custom_private);

void my_ExecIndexBuildScanKeys(PlanState *planstate, Relation index, List *quals, bool isorderby, ScanKey *scanKeys, int *numScanKeys, IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys, IndexArrayKeyInfo **arrayKeys, int *numArrayKeys);


static int pairingheap_SpGISTSearchItem_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg);
void my_spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
void myspgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys);
void my_index_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
IndexScanDesc my_spgbeginscan(Relation rel, int keysz, int orderbysz);
IndexScanDesc myspgbeginscan(Relation rel, int keysz, int orderbysz);
// static IndexScanDesc my_index_beginscan_internal(Relation indexRelation, int nkeys, int norderbys, Snapshot snapshot);
IndexScanDesc my_index_beginscan(Relation heapRelation, Relation indexRelation, Snapshot snapshot, int nkeys, int norderbys);
// static int cmp_orderbyvals(const Datum *adist, const bool *anulls, const Datum *bdist, const bool *bnulls, IndexScanState *node);
// static int reorderqueue_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg);
// static HeapTuple reorderqueue_pop(IndexScanState *node);
// static void EvalOrderByExpressions(IndexScanState *node, ExprContext *econtext);
// static void reorderqueue_push(IndexScanState *node, HeapTuple tuple, Datum *orderbyvals, bool *orderbynulls);
// ItemPointer my_index_getnext_tid(IndexScanDesc scan, ScanDirection direction);
// HeapTuple my_index_getnext(IndexScanDesc scan, ScanDirection direction);
// static void resetSpGistScanOpaque(my_SpGistScanOpaque so);
// static void spgPrepareScanKeys(IndexScanDesc scan);
// static void spgistScanPage(IndexScanDesc scan, SPGISTSearchItem *pageItem);

/* this function scans the index and compute the distance from the bounding boxes computed by compute Bounding Box function*/
// static void spgistScanPage2(IndexScanDesc scan, SPGISTSearchItem *pageItem, bool *);

/* this function should scan the index and compute the distance from the corner points that are calculated on the fly */
static void spgistScanPage3(IndexScanDesc scan, SPGISTSearchItem *pageItem, bool *);

// bool my_spggettuple(IndexScanDesc scan, ScanDirection dir);
bool myspggettuple(IndexScanDesc scan, ScanDirection dir);
// static bool spgistindex_keytest(IndexScanDesc scan, IndexTuple tuple, Page page, OffsetNumber offset, bool *recheck_p, bool *recheck_distances_p);
// static bool spgistindex_keytest_computeDistance(IndexScanDesc scan, SpGistNodeTuple tuple, Page page, OffsetNumber offset, bool *recheck_p, bool *recheck_distances_p);
// static bool my_computeDistance(IndexScanDesc scan, SPGISTSearchItem * item, int which, bool isLeaf , Point * leafVal);
static SPGISTSearchItem * getNextSPGISTSearchItem(mySpGistScanOpaque so);
static bool my_getNextNearest(IndexScanDesc scan);
// static void spgistGetBlock(IndexScanDesc scan, SPGISTSearchItem *pageItem, bool *);
// bool my_spggettuple2(IndexScanDesc scan, ScanDirection dir);

BOX Compute_BoundingBox(ItemPointer itptr, Relation index );
void start_Compute_BoundingBox(Relation index, Oid indexid);
void Compute_BoundingBoxes(Relation index);

PG_FUNCTION_INFO_V1(myspgist_point_distance);
Datum  myspgist_point_distance(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(myspghandler);
Datum myspghandler(PG_FUNCTION_ARGS);

void myspgcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
        Cost *indexStartupCost, Cost *indexTotalCost,
        Selectivity *indexSelectivity, double *indexCorrelation);

//=========================================
//    Catalog builder
//=========================================
//----------------------------
/*
  catalog Builder for kNN-Select Pre-filter
*/

// Build Catalog function 
// Input : index name 

#include "catalog/pg_am.h"
#include "catalog/namespace.h"

#define MAX_NO_LEAF_PAGE 50000
#define MAX_NO_LEAF_OFFSETS 2000
#define MAX_SIZE_KEY_CATALOG 100001
#define MAX_NO_POINTS_BLOCK 10000
#define MAX_K 10000
#define PAIRINGHEAP_DEBUG 

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX) // rd_rel relation typle from pg_class
#define IS_SPGIST(r) ((r)->rd_rel->relam == SPGIST_AM_OID)
#define SPGIST2_AM_OID 65731 // MAC 
//#define SPGIST2_AM_OID 24590 // Linux Server
#define IS_SPGIST2(r) ((r)->rd_rel->relam == SPGIST2_AM_OID)
  

/* instead of storing the catalog as ([K1,K2], cost) 
   store the catalog as an array of K2 values only , and the cost array is the corresponding cost for each K 
   apply binary search on this ordered array to find the cost for the specific k */
typedef struct {
  int size;
  int key[MAX_SIZE_KEY_CATALOG]; 
  int cost[MAX_SIZE_KEY_CATALOG];
} CATALOG;

// all the info and statistics required to be known about the data block
typedef struct {
  /* related to the tree*/
  BlockNumber blkno;      /* index page number, or InvalidBlockNumber */
  OffsetNumber offset[MAX_NO_LEAF_PAGE];
  ItemPointerData ptr[MAX_NO_LEAF_PAGE];    /* block and offset to scan from */
  int level;
  Point P_center; /* parent center */
  Point P_min, P_max; /* corner points of parent bounding box*/

  /* related to the catalog-builder logic*/
  double dist; // distance between this data block and the center of the block we are focus on
  double dist_c1,dist_c2,dist_c3,dist_c4; 
  
  CATALOG catalog_center;
  CATALOG catalog_corner_UL; // upper Left
  CATALOG catalog_corner_UR; // upper Right
  CATALOG catalog_corner_LL; // Lower Left
  CATALOG catalog_corner_LR; // Lower Right
} dataBlock;

typedef struct {
  BlockNumber blkno;      /* index page number, or InvalidBlockNumber */
  OffsetNumber offset;
  ItemPointerData ptr;    /* block and offset to scan from */
  int level;
  Point P_center; /* parent center */
  Point P_min, P_max; /* corner points of parent bounding box*/

} stackItemData;


typedef struct 
{
  pairingheap_node phNode;
  double dist;
  Point p;

} TuplePoint_info;


typedef struct 
{
  pairingheap_node phNode;
  double dist;
  int blkno; // the block no in the data block array so I can retrieve its data 
  int offset;
} DataBlockHeap_info;

void init_Block_arr(void);
void ReadDataBlocks(Relation index, SpGistState *state);
static int pairingheap_dataBlockCenter_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg);
static int pairingheap_TuplePointInfo_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg);

void fill_blockQ(pairingheap * blockQ , Point q );
void FillTupleQ(pairingheap* tupleQ , int p_blkno, int p_offset , Point q, Relation index, SpGistState *state);

void Fill_catalog_center(pairingheap* blockQ,  int blkno, int offset, Point q ,Relation index,  SpGistState *state);
void BuildCatalogLogic(SpGistState *state , Relation index);
void print_catalog(CATALOG* _catalog);
void add_newItem_Catalog(CATALOG* _catalog, int cost , int key , int size);

void print_block_arr(Relation index , SpGistState * state);

void print_pairingheap(pairingheap *heap);
// static void print_pairingheap_recurse( pairingheap_node *node, int depth, pairingheap_node *prev_or_parent);
static TuplePoint_info * getNextTuple(pairingheap* tupleQ);
static DataBlockHeap_info * getNextDataBlock(pairingheap* blockQ);

static TuplePoint_info * Tuple_top(pairingheap* tupleQ);
static DataBlockHeap_info * DataBlock_top(pairingheap* blockQ);

PG_FUNCTION_INFO_V1(build_catalog2);
Datum build_catalog2(PG_FUNCTION_ARGS);

// dataBlock *Block_arr[MAX_NO_LEAF_PAGE][MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
static dataBlock *Block_arr[MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
static Point minP;
static Point maxP;

int FindPoint(Relation index , SpGistState *state , Point * queryPoint);
int myspgWalk(Oid oid , Point * queryPoint, char * indexName);
static bool searchCatalog(CATALOG* _catalog, int k, int *i);
int FindCost_catalog(int blkno , int k);
int FindCost_catalogTbl(char * indexName, BlockNumber blkno, int k);
int FindCost_catalogTbl2(char * indexName, BlockNumber blkno, int k);
int FindCost_catalogTbl_Bin(Oid indexoid, BlockNumber blkno, int k);
int ReadGrid(Oid , Point*);

//==========================================
//    Function implementations
//==========================================

#define KNN_SELECT_PRE_FILTER false
#define KNN_SELECT_POST_FILTER false
#define KNN_JOIN_PRE_FILTER true
#define KNN_JOIN_POST_FILTER false

/*=====================*/

static void my_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
  // elog(NOTICE, "\n\n\n ExecutorStart() - start");
  clock_t start, end;
  double cpu_time_used;
  start = clock();

  standard_ExecutorStart(queryDesc, eflags);
  
  // elog(NOTICE, "ExecutorStart() - Finish\n\n\n");
  end = clock();
  cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
   // elog(NOTICE, "\n\n\n ExecutorStart() - time = %f secs\n\n\n" , cpu_time_used);
  printf("\n%f\n" , cpu_time_used);
}

//Postgresql version 9.6.4
static void my_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
  // elog(NOTICE, "\n\n\n my_ExecutorRun_hook() - start");
  clock_t start, end;
  double cpu_time_used;
  start = clock();

  standard_ExecutorRun(queryDesc, direction, count);
  
  end = clock();
  cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
   // elog(NOTICE, "\n\n\n ExecutorRun() - time = %f secs\n\n\n" , cpu_time_used);
  printf("\n%f\n" , cpu_time_used);
   // elog(NOTICE, "my_ExecutorRun_hook() - Finish\n\n\n");
}

//PostgreSQL version 4
//  void my_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
// {
//     standard_ExecutorRun(queryDesc, direction, count, execute_once);
// }
static void my_create_upper_paths_hook (PlannerInfo *root,
                           UpperRelationKind stage,
                             RelOptInfo *input_rel,
                           RelOptInfo *output_rel)
{
  // printf("\n------------------------------------ my_create_upper_paths_hook:\n\n");
  //  elog(NOTICE , "my_create_upper_paths_hook --------------- start :");
  // switch(stage)
  // {
  //   case UPPERREL_SETOP:        /* result of UNION/INTERSECT/EXCEPT, if any */
  //     printf("UPPERREL_SETOP\n");
  //     break;
  //   case UPPERREL_GROUP_AGG:     /* result of grouping/aggregation, if any */
  //     printf("UPPERREL_GROUP_AGG\n");
  //     elog(NOTICE , "my_create_upper_paths_hook --------------- UPPERREL_GROUP_AGG :");
  //     break;
  //   case UPPERREL_WINDOW:       result of window functions, if any 
  //     printf("UPPERREL_WINDOW\n");
  //     break;
  //   case UPPERREL_DISTINCT:      /* result of "SELECT DISTINCT", if any */
  //     printf("UPPERREL_DISTINCT\n");
  //     break;
  //   case UPPERREL_ORDERED:
  //     printf("UPPERREL_ORDERED\n");
  //     elog(NOTICE , "my_create_upper_paths_hook --------------- UPPERREL_ORDERED :");
  //     break;

  //   case UPPERREL_FINAL:
  //    printf("UPPERREL_FINAL\n");
  //    elog(NOTICE , "my_create_upper_paths_hook --------------- UPPERREL_FINAL:");
  //     //=======================================
      // this is for knn-Join pre-Filter 
      // I need  to cache the output of the knn-Join
      
      // {
      //   RelOptInfo * rel = output_rel;
      //   ListCell * cell2;
      //   Path * p = NULL;
      //   foreach(cell2, rel->pathlist)
      //   {
           
      //     if(IsA( (Path*) lfirst(cell2) , LimitPath))
      //     {
      //       p = (Path *) lfirst(cell2);
      //       break;
      //     }
      //   }

      //   if(p != NULL && IsA( p , LimitPath))
      //   {
      //     rel->pathlist = NIL;//list_delete_first(rel->pathlist);

      //     elog(NOTICE, "\n\nmy_create_upper_paths_hook =========== Add Material Path \n\n");
      //         printf("\n\nMaterial Path 1\n\n");
      //         pprint(p);
              
      //         MaterialPath * pp = create_material_path(rel ,(Path*) p);
      //         printf("\n\nMaterial Path\n\n");
      //         pprint(pp);
      //         add_path(rel, (Path *) pp);
      //         p = NULL;
      //   }
      // }
      //=======================================
      //=======================================

      // this is a trial to add materialze above the relational predicate output
      // to cache the output for pre-Filter KNN-Join
     // {
     //    RelOptInfo * rel = output_rel;
     //    ListCell * cell2;
     //    if(rel->rows == 10000)
     //    {
     //      Path * p = NULL;
     //      foreach(cell2, rel->pathlist)
     //      {
     //          p = (Path *) lfirst(cell2);
     //          elog(NOTICE, "\n\nmy_create_upper_paths_hook =========== Add Material Path \n\n");
     //            printf("\n\nMaterial Path 1\n\n");
     //            pprint(p);
                
     //            MaterialPath * pp = create_material_path(rel ,(Path*) p);
     //            printf("\n\nMaterial Path\n\n");
     //            pprint(pp);
                
     //            p = (Path *) pp;
     //            // add_path(rel, (Path *) pp);
     //            // p = NULL;
     //      }

          
     //    }
     //  }
      //=======================================




    //add cutom path representing K-In-Circle path instead of Limit path: 
    // inputs: K , operator <-> args  
    // int k = 6;
    // OpExpr* KNN_op = NULL;
    // ListCell * lc;
    // ListCell * next = NULL;
    // ListCell * prev = NULL;
    
    // for(lc = list_head(output_rel->pathlist) ; lc != NULL ; lc = next) 
    // {
    //   next = lnext(lc);
    //   if(IsA(lfirst(lc), SubqueryScanPath))
    //   {
    //       SubqueryScanPath * pathnode = lfirst(lc);
    //       Path * subpath = pathnode->subpath;
    //       Path * p = (Path *) create_KInCircle_path(root, output_rel, NULL, KNN_op , k  ,subpath);
          
    //       // TODO : assign pathtarget to input_rel->reltarget OR pass the input_rel instead of output_rel
    //       p->pathtarget = input_rel->reltarget;
          
    //       //^_^
    //       root->glob->subroots = lappend(root->glob->subroots , input_rel->subroot);
          
    //       output_rel->pathlist = list_delete_cell(output_rel->pathlist , lc , prev);
    //       add_path(output_rel , p);
      
    //   }
    //   else
    //     prev = lc;
    // }

  //   break;
  //   default:
  //   break;
  // }
   // printf("\nmy_create_upper_paths_hook==============root\n");
   // pprint(root);
   // printf("\nmy_create_upper_paths_hook==============input_rel\n");
   // pprint(input_rel);
   // printf("\nmy_create_upper_paths_hook==============output_rel\n");
   // pprint(output_rel);

}


//Version 9.6.4
static PlannedStmt *
myplanner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
  // printf("\n============================================\n");
  // printf("=========== myPlanner is called  ===========\n");
  // printf("============================================\n\n");
  
  PlannedStmt *result;
  PlannerGlobal *glob;
  double    tuple_fraction;
  PlannerInfo *root;
  RelOptInfo *final_rel;
  Path     *best_path;
  Plan     *top_plan;
  ListCell   *lp,
         *lr;

  /* Cursor options may come from caller or from DECLARE CURSOR stmt */
  if (parse->utilityStmt &&
    IsA(parse->utilityStmt, DeclareCursorStmt))
    cursorOptions |= ((DeclareCursorStmt *) parse->utilityStmt)->options;

  /*
   * Set up global state for this planner invocation.  This data is needed
   * across all levels of sub-Query that might exist in the given command,
   * so we keep it in a separate struct that's linked to by each per-Query
   * PlannerInfo.
   */
  glob = makeNode(PlannerGlobal);

  glob->boundParams = boundParams;
  glob->subplans = NIL;
  glob->subroots = NIL;
  glob->rewindPlanIDs = NULL;
  glob->finalrtable = NIL;
  glob->finalrowmarks = NIL;
  glob->resultRelations = NIL;
  glob->relationOids = NIL;
  glob->invalItems = NIL;
  glob->nParamExec = 0;
  glob->lastPHId = 0;
  glob->lastRowMarkId = 0;
  glob->lastPlanNodeId = 0;
  glob->transientPlan = false;
  glob->dependsOnRole = false;

  /*
   * Assess whether it's feasible to use parallel mode for this query. We
   * can't do this in a standalone backend, or if the command will try to
   * modify any data, or if this is a cursor operation, or if GUCs are set
   * to values that don't permit parallelism, or if parallel-unsafe
   * functions are present in the query tree.
   *
   * For now, we don't try to use parallel mode if we're running inside a
   * parallel worker.  We might eventually be able to relax this
   * restriction, but for now it seems best not to have parallel workers
   * trying to create their own parallel workers.
   *
   * We can't use parallelism in serializable mode because the predicate
   * locking code is not parallel-aware.  It's not catastrophic if someone
   * tries to run a parallel plan in serializable mode; it just won't get
   * any workers and will run serially.  But it seems like a good heuristic
   * to assume that the same serialization level will be in effect at plan
   * time and execution time, so don't generate a parallel plan if we're in
   * serializable mode.
   */
  glob->parallelModeOK = (cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&
    IsUnderPostmaster && dynamic_shared_memory_type != DSM_IMPL_NONE &&
    parse->commandType == CMD_SELECT && !parse->hasModifyingCTE &&
    parse->utilityStmt == NULL && max_parallel_workers_per_gather > 0 &&
    !IsParallelWorker() && !IsolationIsSerializable() &&
    !has_parallel_hazard((Node *) parse, true);

  /*
   * glob->parallelModeNeeded should tell us whether it's necessary to
   * impose the parallel mode restrictions, but we don't actually want to
   * impose them unless we choose a parallel plan, so it is normally set
   * only if a parallel plan is chosen (see create_gather_plan).  That way,
   * people who mislabel their functions but don't use parallelism anyway
   * aren't harmed.  But when force_parallel_mode is set, we enable the
   * restrictions whenever possible for testing purposes.
   */
  glob->parallelModeNeeded = glob->parallelModeOK &&
    (force_parallel_mode != FORCE_PARALLEL_OFF);

  /* Determine what fraction of the plan is likely to be scanned */
  if (cursorOptions & CURSOR_OPT_FAST_PLAN)
  {
    /*
     * We have no real idea how many tuples the user will ultimately FETCH
     * from a cursor, but it is often the case that he doesn't want 'em
     * all, or would prefer a fast-start plan anyway so that he can
     * process some of the tuples sooner.  Use a GUC parameter to decide
     * what fraction to optimize for.
     */
    tuple_fraction = cursor_tuple_fraction;

    /*
     * We document cursor_tuple_fraction as simply being a fraction, which
     * means the edge cases 0 and 1 have to be treated specially here.  We
     * convert 1 to 0 ("all the tuples") and 0 to a very small fraction.
     */
    if (tuple_fraction >= 1.0)
      tuple_fraction = 0.0;
    else if (tuple_fraction <= 0.0)
      tuple_fraction = 1e-10;
  }
  else
  {
    /* Default assumption is we need all the tuples */
    tuple_fraction = 0.0;
  }

  /* primary planning entry point (may recurse for subqueries) */
  //printf("\n------------------------------------\nparse Query:\n");
  //pprint(parse);
  // printf("\nstandard_planner=================== root before subquery_planner\n");
  // pprint(root);
  // printf("\nstandard_planner=================== parse before subquery_planner\n");
  // pprint(parse);
  root = subquery_planner(glob, parse, NULL,
              false, tuple_fraction);

  // printf("\nstandard_planner=================== root: after subquery_planner\n");
  // pprint(root);
  // printf("\nstandard_planner=================== parse: after subquery_planner \n");
  // pprint(parse);
  
  /* Select best Path and turn it into a Plan */
  final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
  // printf("\n------------------------------------final_rel:\n");
  // pprint(final_rel);
  // // printf("\nstandard_planner=================== root: after fetch_upper_rel\n");
  // pprint(root);
  
 // printf("\nstandard_planner=================== root->upper_rels :\n");
 // pprint(root->upper_rels);
  
  // elog(NOTICE,"============================= standard_planner:  2\n");
  best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);
  
  // elog(NOTICE,"============================= standard_planner:  3\n");
  // printf("\n------------------------------------best_path:\n");
  // pprint(best_path);

  // pre-Filter KNN-Join hack
  // best_path = add_cacheNode(best_path);

  // printf("\nstandard_planner=================== root: after get_cheapest_fractional_path\n");
  // pprint(root);
  
  // elog(NOTICE , "Planner ======================= 0.0 ");
  top_plan = create_plan(root, best_path);
  // elog(NOTICE , "Planner ======================= 0.01 ");
  // elog(NOTICE,"============================= standard_planner:  4\n");

  // printf("\n------------------------------------\nfinal_rel standard_planner:\n");
  // pprint(final_rel);

  // printf("\n------------------------------------\nbest_path standard_planner:\n");
  // pprint(best_path);

  // printf("\n------------------------------------\ntop_plan standard_planner:\n");
  // pprint(top_plan);


  /*
   * If creating a plan for a scrollable cursor, make sure it can run
   * backwards on demand.  Add a Material node at the top at need.
   */
  if (cursorOptions & CURSOR_OPT_SCROLL)
  {
    if (!ExecSupportsBackwardScan(top_plan))
      top_plan = materialize_finished_plan(top_plan);
  }

  // elog(NOTICE , "Planner ======================= 0.02 ");
  /*
   * Optionally add a Gather node for testing purposes, provided this is
   * actually a safe thing to do.  (Note: we assume adding a Material node
   * above did not change the parallel safety of the plan, so we can still
   * rely on best_path->parallel_safe.)
   */
  if (force_parallel_mode != FORCE_PARALLEL_OFF && best_path->parallel_safe)
  {
    Gather     *gather = makeNode(Gather);

    gather->plan.targetlist = top_plan->targetlist;
    gather->plan.qual = NIL;
    gather->plan.lefttree = top_plan;
    gather->plan.righttree = NULL;
    gather->num_workers = 1;
    gather->single_copy = true;
    gather->invisible = (force_parallel_mode == FORCE_PARALLEL_REGRESS);

    /*
     * Ideally we'd use cost_gather here, but setting up dummy path data
     * to satisfy it doesn't seem much cleaner than knowing what it does.
     */
    gather->plan.startup_cost = top_plan->startup_cost +
      parallel_setup_cost;
    gather->plan.total_cost = top_plan->total_cost +
      parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
    gather->plan.plan_rows = top_plan->plan_rows;
    gather->plan.plan_width = top_plan->plan_width;
    gather->plan.parallel_aware = false;

    /* use parallel mode for parallel plans. */
    root->glob->parallelModeNeeded = true;

    top_plan = &gather->plan;
  }

  // elog(NOTICE , "Planner ======================= 0.03 ");
  /*
   * If any Params were generated, run through the plan tree and compute
   * each plan node's extParam/allParam sets.  Ideally we'd merge this into
   * set_plan_references' tree traversal, but for now it has to be separate
   * because we need to visit subplans before not after main plan.
   */
  if (glob->nParamExec > 0)
  {
    Assert(list_length(glob->subplans) == list_length(glob->subroots));
    forboth(lp, glob->subplans, lr, glob->subroots)
    {
      Plan     *subplan = (Plan *) lfirst(lp);
      PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);

      SS_finalize_plan(subroot, subplan);
    }
    SS_finalize_plan(root, top_plan);
  }

  /* final cleanup of the plan */
  Assert(glob->finalrtable == NIL);
  Assert(glob->finalrowmarks == NIL);
  Assert(glob->resultRelations == NIL);
  
  // printf("Planner =======================. top_plan before: \n");
  // pprint(top_plan);

  // printf("Planner =======================. root before: \n");
  // pprint(root);

  // elog(NOTICE , "Planner ======================= 0.1 ");
  // printf("Planner =======================. root->glob->finalrtable before: \n");
  // pprint(root->glob->finalrtable);

  top_plan = set_plan_references(root, top_plan);

  // elog(NOTICE , "Planner ======================= 0.2 ");
  // printf("\nstandard_planner =======================. top_plan after: \n");
  // pprint(top_plan);
  // printf("\nstandard_planner =======================. root: \n");
  // pprint(root);
  
  /* ... and the subplans (both regular subplans and initplans) */
  Assert(list_length(glob->subplans) == list_length(glob->subroots));
  forboth(lp, glob->subplans, lr, glob->subroots)
  {
    //printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\nsubplan before: \n");
    
    Plan     *subplan = (Plan *) lfirst(lp);
    //pprint(subplan);
    
    PlannerInfo *subroot = (PlannerInfo *) lfirst(lr);

    lfirst(lp) = set_plan_references(subroot, subplan);

    //printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\nsubplan after: \n");
    //pprint((Plan *) lfirst(lp));
  }

  //DEBUG
  //=============================================================================
  //==========  This part for replacing subquery node with a custom node
  //=============================================================================

  /* do set_refs for the customPath that replaces subquery */
  // elog(NOTICE, "standard_planner===================  call set_plan_references for subquery");
  // elog(NOTICE, "standard_planner===================  1");
  CustomScan * customPlan = NULL;
  Plan * customsubplan = NULL;
  List * customsubplanModified = NIL;

  List * custom_plans = NIL;
  walkPlan( top_plan, &custom_plans );
  // customPlan = (CustomScan *)walk( top_plan);
  // printf("\nstandard_planner===================  custom_plans from walkPlan():\n");
  // pprint(custom_plans);
  
  // printf("\nstandard_planner===================  root->simple_rel_array: root->simple_rel_array_size = %d\n", root->simple_rel_array_size);
  // int k=0;
  // for(; k< root->simple_rel_array_size; k++)
  //   pprint(root->simple_rel_array[k]);
  
  ListCell * cell;
  foreach(cell , custom_plans)
  {
    // elog(NOTICE, "standard_planner===================  2");
    customPlan = (CustomScan *) lfirst(cell);

    int x = -1;
    // if( (x = bms_first_member(customPlan->custom_relids) ) >= 0 )//if( (x = bms_next_member(customPlan->custom_relids, x)) >= 0)
    if( (x = bms_next_member(customPlan->custom_relids, x)) >= 0)
    {
      // elog(NOTICE, "standard_planner===================  3, x = %d , simple_rel_array_size = %d", x, root->simple_rel_array_size);

      //DEBUG
      // x = 2;
      //DEBUG
      
      // RelOptInfo * rel = find_base_rel(root, x);
      PlannerInfo *subroot = (PlannerInfo *) lsecond(customPlan->custom_private);
      // customsubplan = (Plan*)linitial(customPlan->custom_plans);
      customsubplan = (Plan*)linitial(customPlan->custom_private);

      // if(customsubplan != NULL)
      //   elog(NOTICE, "standard_planner===================  4");
      // else
        // elog(NOTICE, "standard_planner===================  444");

      // printf("\nstandard_planner===================  rel->subroot: \n");
      // pprint(rel->subroot);
      // pprint(subroot);
      
      // customsubplan = set_plan_references(rel->subroot, customsubplan);
      customsubplan = set_plan_references(subroot, customsubplan);

      customsubplanModified = lappend(customsubplanModified , customsubplan);
      // customPlan->custom_plans = lappend(customPlan->custom_plans ,customsubplan );
      // customPlan->custom_private = NIL;

      // printf("\nstandard_planner===================  customsubplan: \n");
      // pprint(customsubplan);
      // printf("\nstandard_planner===================  customPlan: \n");
      // pprint(customPlan);
      // printf("\nstandard_planner===================  top_plan: \n");
      // pprint(top_plan);

      // break;
    }

   
    // elog(NOTICE, "standard_planner===================  6");
    // break;
    // if(!bms_is_empty(customPlan->custom_relids))
    //   bms_free(customPlan->custom_relids);
  }

  // elog(NOTICE, "standard_planner===================  5.1");
  // cell =NULL;
  // foreach(cell, custom_plans)
  // {
  //   elog(NOTICE, "standard_planner===================  5.2");
  //   customPlan = (CustomScan *) lfirst(cell);
  //   elog(NOTICE, "standard_planner===================  5.3");
  //   customsubplan = (Plan*)linitial(customPlan->custom_private);
  //   elog(NOTICE, "standard_planner===================  5.4");
  //   customPlan->custom_plans = lappend(customPlan->custom_plans ,customsubplan );
  //   customPlan->custom_private = NIL;
  //   elog(NOTICE, "standard_planner===================  5.5");
  // }

  customsubplan = NULL;
  ListCell* cell1 =NULL, *cell2 = NULL;
  forboth(cell1, custom_plans , cell2 ,customsubplanModified )
  {
    // elog(NOTICE, "standard_planner===================  5.2");
    
    customPlan = (CustomScan *) lfirst(cell1);
    customsubplan = (Plan *) lfirst(cell2);
    
    // elog(NOTICE, "standard_planner===================  5.3");
    
    // customsubplan = (Plan*)linitial(customPlan->custom_private);
    
    // elog(NOTICE, "standard_planner===================  5.4");
    customPlan->custom_plans = NIL;
    customPlan->custom_plans = lappend(customPlan->custom_plans ,customsubplan );
    customPlan->custom_private = NIL;


    // printf("\nstandard_planner===================  customPlanModifed: \n");
    // pprint(customPlan);
    // elog(NOTICE, "standard_planner===================  5.5");

  }
  //=============================================================================
  //=============================================================================
  //DEBUG
  
  /* build the PlannedStmt result */
  result = makeNode(PlannedStmt);

  result->commandType = parse->commandType;
  result->queryId = parse->queryId;
  result->hasReturning = (parse->returningList != NIL);
  result->hasModifyingCTE = parse->hasModifyingCTE;
  result->canSetTag = parse->canSetTag;
  result->transientPlan = glob->transientPlan;
  result->dependsOnRole = glob->dependsOnRole;
  result->parallelModeNeeded = glob->parallelModeNeeded;
  result->planTree = top_plan;
  result->rtable = glob->finalrtable;
  result->resultRelations = glob->resultRelations;
  result->utilityStmt = parse->utilityStmt;
  result->subplans = glob->subplans;
  result->rewindPlanIDs = glob->rewindPlanIDs;
  result->rowMarks = glob->finalrowmarks;
  result->relationOids = glob->relationOids;
  result->invalItems = glob->invalItems;
  result->nParamExec = glob->nParamExec;

   // printf("\nstandard_planner===================  plannedStmt: \n");
   // pprint(result);

  return result;
}



static RelOptInfo * my_join_search_hook(PlannerInfo *root, 
                            int levels_needed,
                            List *initial_rels)
{
  // printf("\n------------------------------------my_join_search_hook start\n");
  // printf("\n------------------------------------root:\n");
  // pprint(root);
  // printf("\n------------------------------------levels_needed: %d" , levels_needed);
  // printf("\n------------------------------------initial_Rels:\n");
  // pprint(initial_rels);

  // //=======================================
  // // this is for knn-Join pre-Filter 
  // // I need  to cache the output of the knn-Join
  // ListCell * cell;
  // foreach(cell, initial_rels)
  // {
  //   RelOptInfo * rel = (RelOptInfo *) lfirst(cell);
  //   ListCell * cell2;
  //   foreach(cell2, rel->pathlist)
  //   {
  //     SubqueryScanPath * p = (SubqueryScanPath *) lfirst(cell2);
  //     if(IsA(p, SubqueryScanPath))
  //     {
  //       if(IsA(p->subpath , LimitPath))
  //       {
  //         /* then add the materialize path to this subpath */
  //         /* TODO: I need to make more check if it's a knn-Join operation or not */
  //         elog(NOTICE, "\n\nmy_join_search_hook =========== Add Material Path \n\n");
  //         MaterialPath * pp = create_material_path(rel ,(Path*) p);
  //         printf("\n\nMaterial Path\n\n");
  //         pprint(pp);
  //         add_path(rel, (Path *) pp);
  //       }
  //     }
  //   }
  // }
  // //=======================================

  RelOptInfo * rel =  standard_join_search(root, levels_needed, initial_rels);

  // printf("\n------------------------------------rel After standard_join_search:\n");
  // pprint(rel);
  /* Pre-Filter KNN-Join handle */
  /* 
      TODO: instead of checking if there is a knn-operator is used, 
           I can check if there is a custom path as a subnode
  */

  // List * newpathlist = NIL;
  // JoinPath * p = NULL;
  // ListCell * cell;
  // foreach(cell, rel->pathlist)
  // {
  //   p = (JoinPath *) lfirst(cell);
  //   // Path     *outerp = p->outerjoinpath;
  //   // Path     *innerp = p->innerjoinpath;

  //   //----------------------
  //   if(IsA(p->outerjoinpath ,SubqueryScanPath) || IsA(p->innerjoinpath ,SubqueryScanPath) )
  //   {
  //     /*should replace this node with a custom node */

  //     newpathlist = lappend(newpathlist, 
  //       create_cacheCustomScan_path(root, rel , 
  //                                   list_make2(p->outerjoinpath, p->innerjoinpath)) );
  //   }
  //   else /* keep the path as it is */ 
  //     newpathlist = lappend(newpathlist, p ); 
  //   //----------------------
  //   //DEBUG
  //   // if(IsA(p ,IndexPath) )
  //   //   newpathlist = lappend(newpathlist, p );
  //   // //DEBUG
  //   // else
  //   //   newpathlist = lappend(newpathlist, create_cacheCustomScan_path(root, rel , list_make1(p)) );
  // }
  // rel->pathlist = NIL;
  // rel->pathlist = newpathlist;

  //========================================
  //========================================
  // check if Ordered Path is selected as a cheapest path
  // ListCell *l;
  // bool KNNoperator = false;
  // foreach(l, rel->pathlist)
  // {
  //   JoinPath * p = lfirst(l);
  //   Path     *outerp = p->outerjoinpath;
  //   Path     *innerp = p->innerjoinpath;

  //   ListCell * outerl;
  //   foreach(outerl , outerp->pathkeys)
  //   {
  //     PathKey * pk = lfirst(outerl);
  //     EquivalenceClass * pk_eclass = pk->pk_eclass;

  //     ListCell *lm;
  //     foreach(lm , pk_eclass->ec_members)
  //     {
  //       EquivalenceMember * emember = lfirst(lm);

  //       if(IsA(emember->em_expr , OpExpr))
  //       {
  //         OpExpr * expr = (OpExpr *)emember->em_expr;
  //         if(expr->opno == 517 && list_length(expr->args) == 2 ) 
  //         {
  //           Var      *leftvar;
  //           Const    *rightvar;
  //           leftvar = (Var *) get_leftop((Expr *) expr);
  //           rightvar = (Const *) get_rightop((Expr *) expr);

  //           if( IsA(rightvar , Const) && rightvar->consttype == 600 && !rightvar->constisnull)
  //           {
  //             elog(NOTICE, "\n\nmy_join_search_hook ================ add pathkey to the JoinPath \n\n");
  //             // p->pathkeys
  //             if(!list_member_ptr(p->path.pathkeys , pk))
  //               {
  //                 elog(NOTICE, "my_join_search_hook ------------ 1 ");
  //                 // p->path.pathkeys = lappend(p->path.pathkeys , pk);
  //                 // p->path.pathkeys = lappend(p->path.pathkeys , root->query_pathkeys);
  //               }
  //             // root->parse->sortClause = NIL;
  //           }
  //         }
  //       }
  //     }

  //   }

  // }
  //========================================
  //========================================
  /* add the optimization of pre_Filter KNN-Join "K-In-Circle" */

  /* TODO: do some checks on the pathlist to make sure it's a 
           pre-Filter KNN-Join operator
           Also, this K-In-Circle Join node should be added when the inner node is
           the "MaterialCustomPath" 
   */

  // JoinPath * joinp = NULL;
  // // KInCircleJoinPath * kInCirclePath = NULL;
  // CustomPath * kInCirclePath = NULL;
  // ListCell * cell;
  // foreach(cell, rel->pathlist)
  // {
  //   joinp = (JoinPath *) lfirst(cell);
  //   Path     *outerp = joinp->outerjoinpath;
  //   Path     *innerp = joinp->innerjoinpath;

  //   // if(IsA(joinp , NestPath) && IsA(innerp, CustomPath)) // this when Materialize Node is added
  //   if(IsA(joinp , NestPath) && IsA(innerp, SubqueryScanPath)) 
  //   {
  //     elog(NOTICE, "my_join_search_hook ---------------- add kInCircleJoinPath");
      
  //     /* version 1 & 2*/
  //     // kInCirclePath = create_kInCircle_Join_path(root, joinp);
      
  //     /* version 3 */
  //     SubqueryScanPath * innerp_Subqp = (SubqueryScanPath *) innerp;    
  //     kInCirclePath = create_kInCircle_Join_path2(root, joinp, innerp_Subqp->subpath);    
      
  //   }
  // }

  // if(kInCirclePath != NULL)
  // {
  //   rel->pathlist = NIL;
  //   add_path(rel, (Path *) kInCirclePath);
  // }
  


  //========================================
  //      Optimization A 
  //========================================
  /* Generic Materialized node */
  // STEP 1 : walk the path list, until find the custom path I've added 
  // STEP 2 : pull up this custom path to be in the inner path for this Join
  // STEP 3 : modify the custom path and add the old inner path of this join 
  //          to the cutom_paths
  // STEP 4 : remove the cutom path node from the path list (that exists down )
  //          if this path was a child for Join node, then by pulling up the 
  //          custom node , all the rest of nodes are pushed down , 
  //          so the join should be removed ?

  /* Another proposal */
  // STEP 1 : create a custom Node where it has hash index table 
  //          (saving for each customer key , whether it qualifys the predicates) 
  // STEP 2 : traverse the pathlist until I found the custom path 
  //          I've generated above Ordered index scan and let this node points to the 
  //          custom node(caching ) I've generated (so this node can access its state ,
  //           and can access the hash index to check whether the generated custkey is available in the hash table or not)
  // STEP 3 : Add the custom node (caching) to the inner path of the Join
  //          and let the custom node (caching) points to the old inner path of the join
  // STEP 4 : The custom Node (caching) ,Algorithm: 
  /*  - check state , if state->Entry avaialble: 
          return it (No execution of the subpath)
      - checke state, if  state->Entry not exists
          - execute subpath 
          - save the result in Materialized table
          - return the result
  */
  // STEP 5 : The custom Node (Ordered Index Scan) ,Algorithm: 
  /*  - result = Execute subpath
      - if result found in the hash index (Custom node (cache))
          - set custom Node (cache) state->Entry = avaialble 
          - return NULL (this will cause all the above parent nodes abort execution and return null)
      - else
          - set custom Node (cache) state->Entry = not exists 
            (this will cause the custom node (cache) to save the result of the output of the subplan)
          - return result    
  */

  // elog(NOTICE, "my_join_search_hook ========== 1 ");

  ListCell * cell;
  foreach(cell, rel->pathlist)
  {

    if(!IsA(lfirst(cell) , NestPath))
      continue;

    // elog(NOTICE, "my_join_search_hook ========== 2 ");
    
    JoinPath * joinPathNode = (JoinPath*) lfirst(cell);

    if(!IsA(joinPathNode->innerjoinpath , SubqueryScanPath)) // TODO: should handle any type of inner path
        continue;
    
    Path * oldInnerPath = joinPathNode->innerjoinpath;
    
    // elog(NOTICE, "my_join_search_hook ========== 3 ");
    // STEP 2
    CustomPath * customPathNode = (CustomPath *)walkPath((Path *) joinPathNode); // TODO: I need to loop over all the paths in rel

    if(customPathNode != NULL)
    {
        
        // elog(NOTICE, "my_join_search_hook ========== 4 ");
        // STEP 1  : create the custom Node (caching)
        CustomPath * customPathNodeCache = create_CacheGenericCustom_path( oldInnerPath , NULL);

        // elog(NOTICE, "my_join_search_hook ========== 4.1 ");
        //DEBUG
           RelOptInfo * rel2 = find_base_rel(root, customPathNodeCache->path.parent->relid);
        //DEBUG
        // elog(NOTICE, "my_join_search_hook ========== 4.2 ");

        // printf("\nmy_join_search_hook----------rel BEFORE:\n");
        // pprint(rel);
        // printf("\nmy_join_search_hook----------rel2 BEFORE:\n");
        // pprint(rel2);
        
        // STEP 3  : add to the inner path of the join 
        joinPathNode->innerjoinpath = (Path *) customPathNodeCache;
        
        //DEBUG
           // RelOptInfo * rel2 = find_base_rel(root, customPathNodeCache->path.parent->relid);
          // elog(NOTICE, "my_join_search_hook ========== 4.3 ");
           rel2->pathlist = NIL;
          add_path( rel2, (Path*) customPathNodeCache) ;
          
          // elog(NOTICE, "my_join_search_hook ========== 4.4 "); 
           
           if( IsA( rel2->cheapest_total_path , SubqueryScanPath ) )
              rel2->cheapest_total_path = (Path*) customPathNodeCache; //rel2->pathlist;
           
           // elog(NOTICE, "my_join_search_hook ========== 4.5 ");

           if( IsA( (Path *) linitial(rel2->cheapest_parameterized_paths) , SubqueryScanPath ) )
              rel2->cheapest_parameterized_paths = rel2->pathlist;
           
           // elog(NOTICE, "my_join_search_hook ========== 4.6 ");

           if( rel2->cheapest_startup_path && IsA( rel2->cheapest_startup_path , SubqueryScanPath ) )
           rel2->cheapest_startup_path = (Path*) customPathNodeCache; //rel2->pathlist;
        //DEBUg

        // printf("\nmy_join_search_hook----------rel AFTER:\n");
        // pprint(rel);
        // printf("\nmy_join_search_hook----------rel2 AFTER:\n");
        // pprint(rel2);
        // elog(NOTICE, "my_join_search_hook ========== 5 ");
      }
  }

  //========================================
  //========================================

  // printf("\n------------------------------------my_join_search_hook 2\n");
  // printf("\n------------------------------------root:\n");
  // pprint(root);
  // printf("\n------------------------------------rel:\n");
  // pprint(rel);
  // printf("\n------------------------------------my_join_search_hook Finish\n");

  return rel;
}

static void my_set_join_pathlist_hook(PlannerInfo * root, 
                                      RelOptInfo * joinrel, 
                                      RelOptInfo * outerrel, 
                                      RelOptInfo * innerrel,
                                      JoinType jointype,
                                      JoinPathExtraData *extra)
{
  // printf("\n------------------------------------my_set_join_pathlist_hook start\n");
  // printf("\n------------------------------------joinrel:\n");
  // pprint(joinrel);
  // printf("\n------------------------------------outerrel:\n");
  // pprint(outerrel);
  // printf("\n------------------------------------innerrel:\n");
  // pprint(innerrel);
  // printf("\n------------------------------------my_set_join_pathlist_hook Finish\n");
  
}

static void my_set_relpathlist(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{
  //DEBUG
  // Query    *parse = root->parse;
  // Query    *subquery = rte->subquery;
  // printf("\n\n------------------------------------ my_set_relpathlist:\n\n");
  // printf("\n------------------------------------Root:\n");
  // pprint(root);
  // printf("\n------------------------------------Rel:\n");
  // pprint(rel);
  // printf("\n------------------------------------root->parse->jointree:\n");
  // pprint(parse->jointree);
  
  // return;
  // //DEBUG
  // my_set_relpathlist2(root, rel, rti, rte);
  // my_set_relpathlist3(root,rel,rti,rte);
  // my_set_relpathlist4(root,rel,rti,rte);
  // my_set_relpathlist5(root,rel,rti,rte);
  my_set_relpathlist6(root,rel,rti,rte);
  return;
  
}


//modify the planner for pre-Filter KNN-Join
void my_set_relpathlist4(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{

  /* TODO: should replace each subqueryScan node with a CustomPath ,
            Then each Join Node that has Custom Node should be replaced with Custom Node
  */
  /* add custom path above each path in the rel pathlist */
  List * newpathlist = NIL;
  Path * p = NULL;
  ListCell * cell;
  foreach(cell, rel->pathlist)
  {
    p = (Path *) lfirst(cell);
    //----------------------
    if(IsA(p ,SubqueryScanPath) &&  IsA ( ((SubqueryScanPath *)p)->subpath , LimitPath) )
    {
      /*should add CustomNode above this node */
       
      // newpathlist = lappend(newpathlist, create_cacheCustomScan_path(root, rel , list_make1(p)) );
      // newpathlist = lappend(newpathlist, replace_cacheCustomScan_path(root, rel , p, list_make1( ((SubqueryScanPath*)p)->subpath ) ));
    }
    else /* keep the path as it is */ 
      newpathlist = lappend(newpathlist, p ); 
    //----------------------
    // DEBUG
    // if(IsA(p ,IndexPath) )
    //   {
    //     elog(NOTICE, "\nmy_set_relpathlist4 =============== print Indexinfo->indexkeys\n\n");
    //     printf("my_set_relpathlist4 ==========  Indexinfo->indexkeys: %d\n", ((IndexPath*)p)->indexinfo->indexkeys[0]);

    //     pprint( ((IndexPath*)p)->indexinfo->indexkeys);
    //   }
    // DEBUG
    // else
    //   newpathlist = lappend(newpathlist, create_cacheCustomScan_path(root, rel , list_make1(p)) );
  }
  rel->pathlist = NIL;
  rel->pathlist = newpathlist;


  // if(rel->baserestrictinfo != NULL )//&& rel->rows > 750000) // I mean orders table
  // {
  //   // this is a trial to add materialze above the relational predicate output
  //     // to cache the output for pre-Filter KNN-Join
  //    {
  //       // RelOptInfo * rel = output_rel;
  //       ListCell * cell2;
  //       List * newpathlist = NIL;
  //       // if(rel->rows == 10000)
  //       {
  //         Path * p = NULL;
  //         foreach(cell2, rel->pathlist)
  //         {
  //             p = (Path *) lfirst(cell2);
  //             elog(NOTICE, "\n\nmy_set_relpathlist4 =========== Add Material Path \n\n");
  //               printf("\n\nmy_set_relpathlist4====== Material Path 1:\n\n");
  //               pprint(p);
                
  //               MaterialPath * pp = create_material_path(rel ,(Path*) p);
  //               printf("\n\nMaterial Path\n\n");
  //               pprint(pp);
                
  //               newpathlist = lappend(newpathlist , pp);
  //               // p = (Path *) pp;
  //               // add_path(rel, (Path *) pp);
  //               // p = NULL;
  //         }
  //         printf("my_set_relpathlist4=========== rel after modification:\n");
  //         rel->pathlist = NIL;
  //         rel->pathlist = newpathlist;
  //         // rel->pathlist = list_copy(newpathlist);
  //         pprint(rel);

          
  //       }
  //     }
  // }
  
}

//=============================================================================
//==========  These functions for replacing subquery node with a custom node
//=============================================================================

/* modify the planner for pre-Filter KNN-Join 
   the k-in-circle Join operator 
 */ 
void my_set_relpathlist5(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{
  elog(NOTICE, "my_set_relpathlist5------------- start");

  bool PreFilterKNNJoin = false;
  CustomPath * materialCustomPath = NULL;

  ListCell * cell;
  foreach(cell, rel->pathlist)
  {
    SubqueryScanPath * subquerypath = (SubqueryScanPath *) lfirst(cell);
    if(IsA(subquerypath , SubqueryScanPath) && subquerypath->path.parent->baserestrictinfo)
    {
      if(IsA(subquerypath->subpath , LimitPath))
      {
        LimitPath * limitp = (LimitPath *) subquerypath->subpath;
        if(IsA(limitp->subpath , IndexPath)) /* TODO: do more checks to make sure the index path is a <-> operator , and it has parameterized operand from outer relation in the knn-Join*/
        {
          PreFilterKNNJoin = true;
          elog(NOTICE, "my_set_relpathlist5------------- 1, add the MaterialPath");
          /* Add the customPath that represent the Materialize step */
          materialCustomPath = create_materialCustom_path2((Path*)subquerypath, /*current path to copy from */
                                     limitp->subpath /* child Path */);
          
          
          // return;
        }
      } 
    }
  }

  if(PreFilterKNNJoin && materialCustomPath != NULL)
  {
    /* TODO: need to compute the Material Cost, so I can choose which path to choose */
    //DEBUG
    rel->pathlist = NIL;
    //DEBUG
    add_path(rel, (Path *) materialCustomPath);
    
    elog(NOTICE, "my_set_relpathlist5------------- 1, add the MaterialPath Done");
    printf("\nmy_set_relpathlist5--------------rel After MAterial PAth:\n");
    pprint(rel);
  }
}

/* this one to replace the subquery Node */
CustomPath* create_materialCustom_path2(Path* currentPath , Path* childPath)
{
  elog(NOTICE, "create_materialCustom_path2 ----------- start , relid= %d " , currentPath->parent->relid);
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  
  RelOptInfo * rel = currentPath->parent;

  Assert(rel->rtekind == RTE_SUBQUERY) ;

  pathnode->path.parent = rel;
  
  // pathnode->custom_paths = lappend(pathnode->custom_paths, childPath);    
  pathnode->path.param_info = currentPath->param_info; 

  pathnode->path.pathtarget = currentPath->pathtarget;
  pathnode->path.parallel_aware = false;
  pathnode->path.parallel_safe = currentPath->parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
  pathnode->path.parallel_workers = currentPath->parallel_workers;
  pathnode->path.pathkeys = currentPath->pathkeys; // ?? or should I put the child pathkeys
  
  /* compute the cost */
  pathnode->path.rows = currentPath->rows;     /* estimated number of result tuples */
  pathnode->path.startup_cost = currentPath->startup_cost; /* cost expended before fetching any tuples */
  pathnode->path.total_cost = currentPath->total_cost;   /* TODO: need to estimate the cost ?? */

  /* set the value of K (Limit Value) */
  SubqueryScanPath * currSubqueryPath = (SubqueryScanPath *) currentPath;
  Assert(IsA(currSubqueryPath, SubqueryScanPath));
  Assert(IsA(currSubqueryPath->subpath, LimitPath));
  
  /* TODO: for current, there is only one limit in the query so definitly, 
            root->limit_count , and limitOffset are representing the K */
  
  /* TODO: I need to put the child path not the subpath of the subquery path */
  pathnode->custom_private =lappend(pathnode->custom_private , ((SubqueryScanPath *)currentPath)->subpath); 
  
  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "MaterialCustomScan";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_MaterialCustomPath;
  pathnode->methods = methods;
  
  

  //DEBUG
  printf("\ncreate_materialCustom_path ============ childPath: \n");
  pprint(childPath);

  printf("\ncreate_materialCustom_path ============ currentPath: \n");
  pprint(currentPath);

  printf("create_materialCustom_path ============= CustomPath:\n");
  pprint(pathnode);
  //DEBUG

  return pathnode;
}


static void
process_subquery_nestloop_params(PlannerInfo *root, List *subplan_params)
{
  ListCell   *ppl;

  foreach(ppl, subplan_params)
  {
    PlannerParamItem *pitem = (PlannerParamItem *) lfirst(ppl);

    if (IsA(pitem->item, Var))
    {
      Var      *var = (Var *) pitem->item;
      NestLoopParam *nlp;
      ListCell   *lc;

      /* If not from a nestloop outer rel, complain */
      if (!bms_is_member(var->varno, root->curOuterRels))
        elog(ERROR, "non-LATERAL parameter required by subquery");
      /* Is this param already listed in root->curOuterParams? */
      foreach(lc, root->curOuterParams)
      {
        nlp = (NestLoopParam *) lfirst(lc);
        if (nlp->paramno == pitem->paramId)
        {
          Assert(equal(var, nlp->paramval));
          /* Present, so nothing to do */
          break;
        }
      }
      if (lc == NULL)
      {
        /* No, so add it */
        nlp = makeNode(NestLoopParam);
        nlp->paramno = pitem->paramId;
        nlp->paramval = copyObject(var);
        root->curOuterParams = lappend(root->curOuterParams, nlp);
      }
    }
    else if (IsA(pitem->item, PlaceHolderVar))
    {
      PlaceHolderVar *phv = (PlaceHolderVar *) pitem->item;
      NestLoopParam *nlp;
      ListCell   *lc;

      /* If not from a nestloop outer rel, complain */
      if (!bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
                 root->curOuterRels))
        elog(ERROR, "non-LATERAL parameter required by subquery");
      /* Is this param already listed in root->curOuterParams? */
      foreach(lc, root->curOuterParams)
      {
        nlp = (NestLoopParam *) lfirst(lc);
        if (nlp->paramno == pitem->paramId)
        {
          Assert(equal(phv, nlp->paramval));
          /* Present, so nothing to do */
          break;
        }
      }
      if (lc == NULL)
      {
        /* No, so add it */
        nlp = makeNode(NestLoopParam);
        nlp->paramno = pitem->paramId;
        nlp->paramval = copyObject(phv);
        root->curOuterParams = lappend(root->curOuterParams, nlp);
      }
    }
    else
      elog(ERROR, "unexpected type of subquery parameter");
  }
}


/* Plan for replace_cacheCustomScan_path */
static Plan * Plan_MaterialCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  elog(NOTICE, "Plan_MaterialCustomPath ----------- start , relid= %d " , rel->relid);
  //DEBUG
  printf("\nPlan_MaterialCustomPath ----------- rel:\n");
  pprint(rel);
  printf("\nPlan_MaterialCustomPath ----------- best_path:\n");
  pprint(best_path);
  printf("\nPlan_MaterialCustomPath ----------- tlist:\n");
  pprint(tlist);
  printf("\nPlan_MaterialCustomPath ----------- clauses:\n");
  pprint(clauses);
  printf("\nPlan_MaterialCustomPath ----------- custom_plans:\n");
  pprint(custom_plans);
  //DEBUG

  /* STEP 1: */
 
  // rel = best_path->path.parent;
  Index   scan_relid = rel->relid;
  Plan     *subplan;

  /* it should be a subquery base rel... */
  Assert(scan_relid > 0);
  Assert(rel->rtekind == RTE_SUBQUERY);

  
  subplan = create_plan(rel->subroot, (Path *)linitial (best_path->custom_private) );

  
  /* STEP 2: */
  clauses = extract_actual_clauses(clauses, false);

  /* STEP 3: */
  if (best_path->path.param_info)
  {
    // clauses = (List *)
    //   replace_nestloop_params(root, (Node *) clauses);
    process_subquery_nestloop_params(root,
                     rel->subplan_params);
  }

  /* STEP 4: */
  CustomScan * customscanNode; 
  
  /* Make the customScan plan node */
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_scan_tlist = tlist;  
  customscanNode->custom_plans = lappend(customscanNode->custom_plans , subplan);
  // customscanNode->custom_exprs = clauses;
  customscanNode->flags = best_path->flags;

  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
 
  plan->lefttree = NULL;//(Plan *)linitial(custom_plans); //custom_plans; ???
  plan->righttree = NULL; // ???

  //DEBUG
  customscanNode->custom_relids = bms_add_member(customscanNode->custom_relids, scan_relid);
  //DEBUG
  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "MaterialCustomScanPlan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_MaterialCustomScan_state;
  customscanNode->methods = methods;

  printf("\nPlan_MaterialCustomPath ----------- customScanNode:\n");
  pprint(customscanNode);
  elog(NOTICE, "Plan_MaterialCustomPath ----------- FINISH");

  return (Plan *) customscanNode;
}

Node *create_MaterialCustomScan_state(CustomScan *node)
{
  elog(NOTICE, "create_MaterialCustomScan_state ------------ start");


  // KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));
  // CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
  // CustomScanState
  /* TODO: new data Structure that contains the priority Queue to hold materialzed points */
  CustomScanState * MaterialCustomScanNode = palloc(sizeof(MaterialCustomScanNode));
  MaterialCustomScanNode->ss.ps.type = T_CustomScanState;
  /*
   * Set the basic state structure 
   */
  MaterialCustomScanNode->custom_ps = NIL;
  MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, linitial (node->custom_plans) );  //list_concat_unique(MaterialCustomScanNode->custom_ps ,
                                        //                node->custom_plans); // these are not planstate yet
  MaterialCustomScanNode->pscan_len = 0;  
  printf("\ncreate_MaterialCustomScan_state ----------- MaterialCustomScanNode->custom_ps:\n");
  pprint(MaterialCustomScanNode->custom_ps);
 
  
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  
  MaterialCustomScanNode->ss.ps.instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.worker_instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.chgParam = NULL; // ???
  MaterialCustomScanNode->ss.ss_currentRelation = NULL;
  // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  
  MaterialCustomScanNode->ss.ps.lefttree = NULL;
  MaterialCustomScanNode->ss.ps.righttree = NULL; 
  MaterialCustomScanNode->ss.ps.initPlan = NULL; //?????
  MaterialCustomScanNode->ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(30));
  name = "MaterialCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_MaterialCustomScan2;
  methods->ExecCustomScan = Exec_MaterialCustomScan;
  methods->EndCustomScan = End_MaterialCustomScan;
  methods->ReScanCustomScan = ReScan_MaterialCustomScan;
  // methods->ExplainCustomScan = Explain_CacheCustomScan;
  
  MaterialCustomScanNode->methods = methods;

  if(MaterialCustomScanNode->methods->BeginCustomScan != NULL)
  {
    elog(NOTICE, "create_MaterialCustomScan_state ------- call BeginCustomScan");
    // MaterialCustomScanNode->methods->BeginCustomScan(MaterialCustomScanNode ,
    //                                                  NULL, 0);
  }

  
  /*
   * TODO: intialize the hash table 
   */

  elog(NOTICE, "create_MaterialCustomScan_state ------------ FINISH");  
  return (Node *) MaterialCustomScanNode;
}

void Begin_MaterialCustomScan2(CustomScanState *node, EState *estate, int eflags)
{
  elog(NOTICE, "Begin_MaterialCustomScan ------------ start");

  PlanState * childplanstate = NULL;
  Plan * childPlan = NULL;

  /* initilaize the custom_exprs */
  CustomScan * plan = (CustomScan *)node->ss.ps.plan;

  // node->ss.ps.qual = (List *)
  //  ExecInitExpr((Expr *) plan->custom_exprs,
  //         (PlanState *) node);

  // if(list_length(node->custom_ps) == 1)
  {
    // childPlan = (Plan *)linitial(node->custom_ps);
    childPlan = (Plan *)linitial (plan->custom_plans);
    childplanstate = ExecInitNode(childPlan, estate, eflags);

    /* initialize children plan state */
    if(childplanstate)
    {
     node->custom_ps = NIL;
     node->custom_ps = lappend(node->custom_ps , childplanstate) ;

     /*
     * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
     */
    ExecAssignScanType(&node->ss,
             ExecGetResultType(childplanstate));
    }
  }
  // else //TODO


  //DEBUG
  // node->
  //DEBUG
  /*
   * Initialize result tuple type and projection info.
   */
  // ExecAssignResultTypeFromTL(&node->ss.ps);
  // ExecAssignScanProjectionInfo(&node->ss);

  
  elog(NOTICE, "Begin_MaterialCustomScan ------------ FINISH");
}


TupleTableSlot *  Exec_MaterialCustomScan (CustomScanState *node)
{ 
  elog(NOTICE, "Exec_MaterialCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  //DEBUG
  pprint(node->ss.ps.state->es_plannedstmt);
  pprint(node->ss.ps.state->es_plannedstmt);
  //DEBUG
  // CacheCustomScanState * CacheNode = (CacheCustomScanState *) node;
  // IndexScanState * indexstate = &knnNode->indexstate;
  return ExecScan(&(node->ss),
          (ExecScanAccessMtd) MaterialCustomNext,
          (ExecScanRecheckMtd) MaterialCustomRecheck);
}

static TupleTableSlot * MaterialCustomNext(CustomScanState *node)
{
  TupleTableSlot *slot = NULL;
  elog(NOTICE, "MaterialCustomNext ---------- start");

  // if(list_length(node->custom_ps) == 1)
    slot = ExecProcNode((PlanState*)linitial( node->custom_ps));
  //else //TODO

  elog(NOTICE, "MaterialCustomNext ---------- finish");
  return slot;
}

static bool MaterialCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true; 
}

void End_MaterialCustomScan (CustomScanState *node)
{
  elog(NOTICE, "End_MaterialCustomScan -------- start");
  
  /*
  * Free the exprcontext
  */
  ExecFreeExprContext(&node->ss.ps);

  /*
  * clean out the upper tuple table
  */
  ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
  * close down subquery
  */

  //End child plans
  // if(list_length(node->custom_ps) == 1)
    ExecEndNode((PlanState*)linitial(node->custom_ps));
  // else //TODO
  elog(NOTICE, "End_MaterialCustomScan -------- finish");
}

void ReScan_MaterialCustomScan (CustomScanState *node)
{

  elog(NOTICE, "ReScan_MaterialCustomScan -------- start");
  ExecScanReScan(&node->ss);

  /*
  * ExecReScan doesn't know about my subplan, so I have to do
  * changed-parameter signaling myself.  This is just as well, because the
  * subplan has its own memory context in which its chgParam state lives.
  */
  PlanState * childplanstate =  (PlanState *)linitial( node->custom_ps);
  if (node->ss.ps.chgParam != NULL)
    UpdateChangedParamSet(childplanstate, node->ss.ps.chgParam);

  /*
  * if chgParam of subnode is not null then plan will be re-scanned by
  * first ExecProcNode.
  */
  if (childplanstate->chgParam == NULL)
    ExecReScan(childplanstate);

  elog(NOTICE, "ReScan_MaterialCustomScan -------- Finish");
}

//============================================================
//============================================================


//=============================================================================
//              Optimization A 
//
//=============================================================================
/* These functions for adding the new custom node to cache the output of the KNN-Join,
    the caching step is for caching whether the tuple qualifies the relational predicate 
    or not, and then if it exists in the cache then no need to execute the relational predicate
    this will be enahned when adding KNN-Join implementation so I can cache the locality of 
    each block number 

        Join
         |
    /         \
 Supplier      Custom Node
                  |
            /           \
          Limit          Subquery Scan
            |                 |
       Ordered Index         Agg + Grouping
           Scan               |
                          Sort (custkey)
                              |
                          Scan Orders table
*/
//=============================================================================

typedef struct CacheHashEntryData *CacheHashEntry;

typedef struct CacheHashEntryData
{
  TupleHashEntryData shared; //  common header for hash table entries 
  
  bool DoesQualify ; /* rach tuple in the hash, whether it qualifies and should be returned to the parent node, or ognore it */
  bool undefinedstatus; 
  TupleTableSlot * tuple; /*tuple it self*/
} CacheHashEntryData;

typedef struct CacheGenericState
{
  CustomScanState css;
  
  /* Hash Table for caching */
  TupleHashTable hashtable; /* hash table with one entry per group */
  TupleTableSlot *hashslot; /* slot for loading hash table */
  List     *hash_needed;  /* list of columns needed in hash table */

  ExprContext *tmpcontext;  /* econtext for input expressions */
  ExprContext *econtexts;  /* econtexts for long-lived data  */
  MemoryContext tupContexts; /* for array */

  FmgrInfo   *hashfunctions;  /*  hash fns */
  FmgrInfo   *eqfunctions;

  long HashTableSize ; /* TODO: this should be the size of the table that ordered index scan is applying for */  
  int numCols ;
  Oid* Operators ; /* equality operators to compare with */
  AttrNumber *grpColIdx;

  /* this is used by the child custom node */
  bool childResultinCache ; /*whenever the returning slot is NULL, */
  bool NeedModification;
  TupleTableSlot * custom_tuple_returned; /* if the entry found in the hash, then the custom node should set this pointer */
  TupleTableSlot *tmpslot;
  CacheHashEntry entryFromChild;
  // Int64 undefinedtuples;
  List * udefinedtuplesKeys; /* pointers to the hash entry that need to be modify */
  List * returnedTuples; /* TupleTableslots that holds the returned tuples */
} CacheGenericState;


//-----------------------------------
static void build_hash_table(CacheGenericState *cachestate);
static CacheHashEntry lookup_hash_entry(CacheGenericState *aggstate, TupleTableSlot *inputslot, bool *);

/* modify the planner for pre-Filter KNN-Join 
   the k-in-circle Join operator 
 */ 
void my_set_relpathlist6(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{
  // elog(NOTICE, "my_set_relpathlist6------------- start , relid = %d" , rel->relid);

  
  bool PreFilterKNNJoin = false;
  CustomPath * materialCustomPath = NULL;

  ListCell * cell;
  foreach(cell, rel->pathlist)
  {
    SubqueryScanPath * subquerypath = (SubqueryScanPath *) lfirst(cell);
    if(IsA(subquerypath , SubqueryScanPath) )//&& subquerypath->path.parent->baserestrictinfo)
    {
      if(IsA(subquerypath->subpath , LimitPath))
      {
        LimitPath * limitp = (LimitPath *) subquerypath->subpath;
        if(IsA(limitp->subpath , IndexPath)) /* TODO: do more checks to make sure the index path is a <-> operator , and it has parameterized operand from outer relation in the knn-Join*/
        {
          PreFilterKNNJoin = true;
          // elog(NOTICE, "my_set_relpathlist6------------- 1, add the MaterialPath");
          /* Add the customPath that represent the Materialize step */
          materialCustomPath = create_materialGenericCustom_path((Path*)subquerypath, /*current path to copy from */
                                     limitp->subpath /* child Path */);
          
          //DEBUG

          //DEBUG
          // return;
        }
      } 
    }
  }

  if(PreFilterKNNJoin && materialCustomPath != NULL)
  {
    /* TODO: need to compute the Material Cost, so I can choose which path to choose */
    // printf("\nmy_set_relpathlist6--------------rel Before MAterial PAth:\n");
    // pprint(rel);

    //DEBUG
    rel->pathlist = NIL;
    //DEBUG
    add_path(rel, (Path *) materialCustomPath);

    //
    // RelOptInfo * rel2 =  find_base_rel(root, rel->relid);
    // printf("\nmy_set_relpathlist6--------------rel2 Before MAterial PAth:\n");
    // pprint(rel2);

    // rel2->pathlist = rel->pathlist;
    
    // elog(NOTICE, "my_set_relpathlist6------------- 1, add the MaterialPath Done");
    // printf("\nmy_set_relpathlist6--------------rel After MAterial PAth:\n");
    // pprint(rel);
    // printf("\nmy_set_relpathlist6--------------rel2 After MAterial PAth:\n");
    // pprint(rel2);
  }

  //=================================

  // bool PreFilterKNNJoin = false;
  // CustomPath * cacheCustomPath = NULL;

  // elog(NOTICE, "my_join_search_hook ========== 1 ");

  // ListCell * cell;
  // foreach(cell, rel->pathlist)
  // {

  //   elog(NOTICE, "my_join_search_hook ========== 2 ");
    
  //   Path * PathNode = (Path*) lfirst(cell);

  //   if(!IsA(joinPathNode, SubqueryScanPath)) // TODO: should handle any type of inner path
  //       continue;
    
  //   Path * oldInnerPath = joinPathNode->innerjoinpath;
    
  //   elog(NOTICE, "my_join_search_hook ========== 3 ");
  //   // STEP 2
  //   CustomPath * customPathNode = (CustomPath *)walkPath((Path *) joinPathNode); // TODO: I need to loop over all the paths in rel

  //   if(customPathNode != NULL)
  //   {
        
  //       elog(NOTICE, "my_join_search_hook ========== 4 ");
  //       // STEP 1  : create the custom Node (caching)
  //       CustomPath * customPathNodeCache = create_CacheGenericCustom_path( oldInnerPath , NULL);

  //       //DEBUG
  //          RelOptInfo * rel2 = find_base_rel(root, customPathNodeCache->path.parent->relid);
  //       //DEBUG

  //       printf("\nmy_join_search_hook----------rel BEFORE:\n");
  //       pprint(rel);
  //       printf("\nmy_join_search_hook----------rel2 BEFORE:\n");
  //       pprint(rel2);
  //       // STEP 3  : add to the inner path of the join 
  //       joinPathNode->innerjoinpath = (Path *) customPathNodeCache;
        
  //       //DEBUG
  //          // RelOptInfo * rel2 = find_base_rel(root, customPathNodeCache->path.parent->relid);

  //          rel2->pathlist = NIL;
  //          add_path( rel2, (Path*) customPathNodeCache) ;
  //       //DEBUg

  //       printf("\nmy_join_search_hook----------rel AFTER:\n");
  //       pprint(rel);
  //       printf("\nmy_join_search_hook----------rel2 AFTER:\n");
  //       pprint(rel2);
  //       elog(NOTICE, "my_join_search_hook ========== 5 ");
  //     }
  // }
}

/* this one to replace the subquery Node */
CustomPath* create_materialGenericCustom_path(Path* currentPath , Path* childPath)
{
  // elog(NOTICE, "create_materialCustom_path2 ----------- start , relid= %d " , currentPath->parent->relid);
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  
  RelOptInfo * rel = currentPath->parent;

  Assert(rel->rtekind == RTE_SUBQUERY) ;

  pathnode->path.parent = rel;
  
  // pathnode->custom_paths = lappend(pathnode->custom_paths, childPath);    
  pathnode->path.param_info = currentPath->param_info; 

  pathnode->path.pathtarget = currentPath->pathtarget;
  pathnode->path.parallel_aware = false;
  pathnode->path.parallel_safe = currentPath->parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
  pathnode->path.parallel_workers = currentPath->parallel_workers;
  pathnode->path.pathkeys = currentPath->pathkeys; // ?? or should I put the child pathkeys
  
  /* compute the cost */
  pathnode->path.rows = currentPath->rows;     /* estimated number of result tuples */
  pathnode->path.startup_cost = currentPath->startup_cost; /* cost expended before fetching any tuples */
  pathnode->path.total_cost = currentPath->total_cost;   /* TODO: need to estimate the cost ?? */

   // set the value of K (Limit Value) 
  SubqueryScanPath * currSubqueryPath = (SubqueryScanPath *) currentPath;
  Assert(IsA(currSubqueryPath, SubqueryScanPath));
  Assert(IsA(currSubqueryPath->subpath, LimitPath));
  
  /* TODO: for current, there is only one limit in the query so definitly, 
            root->limit_count , and limitOffset are representing the K */
  
  /* TODO: I need to put the child path not the subpath of the subquery path */
  pathnode->custom_private =lappend(pathnode->custom_private , ((SubqueryScanPath *)currentPath)->subpath); 
  
  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "MaterialCustomScan";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_MaterialGenericCustomPath;
  pathnode->methods = methods;
  
  

  //DEBUG
  // printf("\ncreate_materialCustom_path ============ childPath: \n");
  // pprint(childPath);

  // printf("\ncreate_materialCustom_path ============ currentPath: \n");
  // pprint(currentPath);

  // printf("create_materialCustom_path ============= CustomPath:\n");
  // pprint(pathnode);

  // printf("create_materialCustom_path ============= rel->subroot:\n");
  // pprint(rel->subroot);
  //DEBUG

  return pathnode;
}


static Plan * Plan_MaterialGenericCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  // elog(NOTICE, "Plan_MaterialCustomPath ----------- start , relid= %d " , rel->relid);
  //DEBUG
  // printf("\nPlan_MaterialCustomPath ----------- rel:\n");
  // pprint(rel);
  // printf("\nPlan_MaterialCustomPath ----------- best_path:\n");
  // pprint(best_path);
  // printf("\nPlan_MaterialCustomPath ----------- tlist:\n");
  // pprint(tlist);
  // printf("\nPlan_MaterialCustomPath ----------- clauses:\n");
  // pprint(clauses);
  // printf("\nPlan_MaterialCustomPath ----------- custom_plans:\n");
  // pprint(custom_plans);
  //DEBUG

  /* STEP 1: */
 
  // rel = best_path->path.parent;
  Index   scan_relid = rel->relid;
  Plan     *subplan;

  /* it should be a subquery base rel... */
  Assert(scan_relid > 0);
  Assert(rel->rtekind == RTE_SUBQUERY);
  Assert(rel->subroot != NULL);
  
  subplan = create_plan(rel->subroot, (Path *)linitial (best_path->custom_private) );

  // printf("\nPlan_MaterialCustomPath ----------- rel->subroot:\n");
  // pprint(rel->subroot);
  /* STEP 2: */
  clauses = extract_actual_clauses(clauses, false);

  /* STEP 3: */
  if (best_path->path.param_info)
  {
    // clauses = (List *)
    //   replace_nestloop_params(root, (Node *) clauses);
    process_subquery_nestloop_params(root,
                     rel->subplan_params);
  }

  // //DEBUG
  //  elog(NOTICE, "Plan_MaterialCustomPath ----------- 1 , set_plan_references : " );
  // subplan = set_plan_references(rel->subroot , subplan);
  // elog(NOTICE, "Plan_MaterialCustomPath ----------- 2 , set_plan_references : " );
  // //DEBUG


  /* STEP 4: */
  CustomScan * customscanNode; 
  
  /* Make the customScan plan node */
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_scan_tlist = tlist;  
  customscanNode->custom_plans = NIL;
  customscanNode->custom_private = NIL;
  customscanNode->custom_private = lappend(customscanNode->custom_private , subplan);
  customscanNode->custom_private = lappend(customscanNode->custom_private , rel->subroot);
  // customscanNode->custom_exprs = clauses;
  customscanNode->flags = best_path->flags;

  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
 
  plan->lefttree = NULL;//(Plan *)linitial(custom_plans); //custom_plans; ???
  plan->righttree = NULL; // ???

  //DEBUG
  // customscanNode->custom_relids = bms_add_member(customscanNode->custom_relids, scan_relid);
  //DEBUG
  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "MaterialCustomScanPlan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_MaterialGenericCustomScan_state;
  customscanNode->methods = methods;

  // printf("\nPlan_MaterialCustomPath ----------- customScanNode:\n");
  // pprint(customscanNode);
  // elog(NOTICE, "Plan_MaterialCustomPath ----------- FINISH");

  return (Plan *) customscanNode;
}

// Node *create_MaterialGenericCustomScan_state(CustomScan *node)
// {
//   elog(NOTICE, "create_MaterialCustomScan_state ------------ start");


//   // KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));
//   // CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
//   // CustomScanState
//   /* TODO: new data Structure that contains the priority Queue to hold materialzed points */
//   CustomScanState * MaterialCustomScanNode = makeNode(CustomScanState); //palloc(sizeof(MaterialCustomScanNode));
//   MaterialCustomScanNode->ss.ps.type = T_CustomScanState;
//   /*
//    * Set the basic state structure 
//    */
//   MaterialCustomScanNode->custom_ps = NIL;
//   MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, (Plan*)linitial (node->custom_plans) );  //list_concat_unique(MaterialCustomScanNode->custom_ps ,
//                                         //                node->custom_plans); // these are not planstate yet
//   // MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, linitial (node->custom_private) );
//   MaterialCustomScanNode->pscan_len = 0;  
//   // printf("\ncreate_MaterialCustomScan_state ----------- MaterialCustomScanNode->custom_ps:\n");
//   // pprint(MaterialCustomScanNode->custom_ps);
 
  
  
//   // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  
//   MaterialCustomScanNode->ss.ps.instrument = NULL; // ???
//   MaterialCustomScanNode->ss.ps.worker_instrument = NULL; // ???
//   MaterialCustomScanNode->ss.ps.chgParam = NULL; // ???
//   MaterialCustomScanNode->ss.ss_currentRelation = NULL;
//   // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  
//   MaterialCustomScanNode->ss.ps.lefttree = NULL;
//   MaterialCustomScanNode->ss.ps.righttree = NULL; 
//   MaterialCustomScanNode->ss.ps.initPlan = NULL; //?????
//   MaterialCustomScanNode->ss.ps.subPlan = NULL;
  
//   /*
//    * Set the custom state structure 
//    */
  
//   struct CustomExecMethods * methods;
//   methods = palloc(sizeof(* methods));
//   char * name = palloc(sizeof(30));
//   name = "MaterialCustomScanState";
//   methods->CustomName = name;
//   methods->BeginCustomScan = Begin_MaterialGenericCustomScan;
//   methods->ExecCustomScan = Exec_MaterialGenericCustomScan;
//   methods->EndCustomScan = End_MaterialGenericCustomScan;
//   methods->ReScanCustomScan = ReScan_MaterialGenericCustomScan;
//   methods->ExplainCustomScan = Explain_MaterialGenericCustomScan;
  
//   MaterialCustomScanNode->methods = methods;

//   if(MaterialCustomScanNode->methods->BeginCustomScan != NULL)
//   {
//     elog(NOTICE, "create_MaterialCustomScan_state ------- call BeginCustomScan");
//     // MaterialCustomScanNode->methods->BeginCustomScan(MaterialCustomScanNode ,
//     //                                                  NULL, 0);
//   }

  
//   /*
//    * TODO: intialize the hash table 
//    */

//   elog(NOTICE, "create_MaterialCustomScan_state ------------ FINISH");  
//   return (Node *) MaterialCustomScanNode;
// }

typedef struct MaterialGenericState
{
  CustomScanState css;
  CacheGenericState * CacheCSS;
} MaterialGenericState;

// This one for using user defined data structure for the customScanState
Node *create_MaterialGenericCustomScan_state(CustomScan *node)
{
  // elog(NOTICE, "create_MaterialCustomScan_state ------------ start");


  MaterialGenericState * MaterialGenericNode =  palloc(sizeof(MaterialGenericState));
  
  /* TODO: new data Structure that contains the priority Queue to hold materialzed points */
  CustomScanState * MaterialCustomScanNode = makeNode(CustomScanState); //palloc(sizeof(MaterialCustomScanNode));
  MaterialCustomScanNode->ss.ps.type = T_CustomScanState;

  MaterialGenericNode->css = * MaterialCustomScanNode;
  MaterialGenericNode->CacheCSS = NULL;
  /*
   * Set the basic state structure 
   */
  MaterialCustomScanNode->custom_ps = NIL;
  MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, (Plan*)linitial (node->custom_plans) );  //list_concat_unique(MaterialCustomScanNode->custom_ps ,
                                        //                node->custom_plans); // these are not planstate yet
  // MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, linitial (node->custom_private) );
  MaterialCustomScanNode->pscan_len = 0;  
  // printf("\ncreate_MaterialCustomScan_state ----------- MaterialCustomScanNode->custom_ps:\n");
  // pprint(MaterialCustomScanNode->custom_ps);
 
  
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  
  MaterialCustomScanNode->ss.ps.instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.worker_instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.chgParam = NULL; // ???
  MaterialCustomScanNode->ss.ss_currentRelation = NULL;
  // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  
  MaterialCustomScanNode->ss.ps.lefttree = NULL;
  MaterialCustomScanNode->ss.ps.righttree = NULL; 
  MaterialCustomScanNode->ss.ps.initPlan = NULL; //?????
  MaterialCustomScanNode->ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(30));
  name = "MaterialCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_MaterialGenericCustomScan;
  methods->ExecCustomScan = Exec_MaterialGenericCustomScan;
  methods->EndCustomScan = End_MaterialGenericCustomScan;
  methods->ReScanCustomScan = ReScan_MaterialGenericCustomScan;
  methods->ExplainCustomScan = Explain_MaterialGenericCustomScan;
  
  MaterialCustomScanNode->methods = methods;

  // if(MaterialCustomScanNode->methods->BeginCustomScan != NULL)
  // {
  //   elog(NOTICE, "create_MaterialCustomScan_state ------- call BeginCustomScan");
  //   // MaterialCustomScanNode->methods->BeginCustomScan(MaterialCustomScanNode ,
  //   //                                                  NULL, 0);
  // }

  
  /*
   * TODO: intialize the hash table 
   */

  // elog(NOTICE, "create_MaterialCustomScan_state ------------ FINISH");  
  return (Node *) MaterialCustomScanNode;
}

void Begin_MaterialGenericCustomScan(CustomScanState *node, EState *estate, int eflags)
{
  // elog(NOTICE, "Begin_MaterialCustomScan ------------ start");

  PlanState * childplanstate = NULL;
  Plan * childPlan = NULL;

  /* initilaize the custom_exprs */
  CustomScan * plan = (CustomScan *)node->ss.ps.plan;

 
    // childPlan = (Plan *)linitial(node->custom_ps);
    childPlan = (Plan *)linitial (plan->custom_plans);
    childplanstate = ExecInitNode(childPlan, estate, eflags);

    /* initialize children plan state */
    if(childplanstate)
    {
     node->custom_ps = NIL;
     node->custom_ps = lappend(node->custom_ps , childplanstate) ;

     /*
     * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
     */
    ExecAssignScanType(&node->ss,
             ExecGetResultType(childplanstate));
    }
  


  
  // elog(NOTICE, "Begin_MaterialCustomScan ------------ FINISH");
}

TupleTableSlot *  Exec_MaterialGenericCustomScan (CustomScanState *node)
{ 
  // elog(NOTICE, "Exec_MaterialCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  //DEBUG
  // pprint(node->ss.ps.state->es_plannedstmt);
  // pprint(node->ss.ps.state->es_plannedstmt);
  //DEBUG
  TupleTableSlot *slot = NULL;
  slot = ExecScan(&(node->ss),
          (ExecScanAccessMtd) MaterialGenericCustomNext,
          (ExecScanRecheckMtd) MaterialGenericCustomRecheck);



  return slot;
}

// version 1, not qorking correctly
// static TupleTableSlot * MaterialGenericCustomNext(CustomScanState *node)
// {
//   TupleTableSlot *slot = NULL;
//   // elog(NOTICE, "MaterialCustomNext ---------- start");

//   while(true)
//   {
//     slot = NULL;
//     slot = ExecProcNode((PlanState*)linitial( node->custom_ps));
    
//     // elog(NOTICE, "MaterialCustomNext ---------- 1");
//     //============================================
//     /* this part to check if the returned slot exists in the cache hash index table in the 
//        cache custom node */
//     MaterialGenericState * tmp = (MaterialGenericState * ) node; 
          
//     CacheGenericState * CacheNode = (CacheGenericState *) tmp->CacheCSS;
//     elog(NOTICE, "MaterialCustomNext ---------- Cache: es_processed : %lu ", tmp->css.ss.ps.state->es_processed);
//     elog(NOTICE, "MaterialCustomNext ---------- Material: es_processed : %lu ", tmp->css.ss.ps.state->es_processed);

//     // initialization 
//     CacheNode->entryFromChild = NULL;
//     CacheNode->childResultinCache = false;
//     if(CacheNode->tmpslot != NULL && CacheNode->tmpslot->tts_tupleDescriptor != NULL)
//       ExecStoreAllNullTuple(CacheNode->tmpslot);

//     if(!TupIsNull(slot))
//     {
//       // STEP 1: search the tuple if exists in the hash
//       // STEP 2: if isnew ? then it wasn't exist before, the tuple was added 
//                 // 1. set tupleinHash , set set ! qualify (by default) 
//                 // 2. for future, I can set the custom_tuple_returned, then the cache node will take it and update the Qualify attribute only, instead of searching from the begning
//       // STEP 3: if not new, was existent before
//                 // 1. set custom_tuple_returned
//                 // 2. set the flags needed
//                 // 3. return null
      

//     // DEBUG
//     if(slot->tts_values[0] == 1237)
//       elog(NOTICE, "MaterialCustomNext ------- DEBUG DEBUG DEBUG");
//     // DEBUG

//       // elog(NOTICE, "MaterialCustomNext ---------- 2");
//       // ExprContext *tmpcontext;
//       CacheHashEntry entry = NULL;
//       TupleTableSlot *firstSlot ;

//       // tmpcontext = CacheNode->tmpcontext;
//       firstSlot = CacheNode->css.ss.ss_ScanTupleSlot;

//       // elog(NOTICE, "MaterialCustomNext ---------- 3");
//       /* build a new slot with the same desc as cache node child */
      
//       if(CacheNode->hashslot->tts_tupleDescriptor == NULL && firstSlot->tts_tupleDescriptor != NULL)
//       { 
//         elog(NOTICE, "MaterialCustomNext ---------- 4");
//         ExecSetSlotDescriptor(CacheNode->hashslot, firstSlot->tts_tupleDescriptor);
//         elog(NOTICE, "MaterialCustomNext ---------- 5");
//         ExecStoreAllNullTuple(CacheNode->hashslot);
//         elog(NOTICE, "MaterialCustomNext ---------- 6");
//       }

//       if(CacheNode->tmpslot->tts_tupleDescriptor == NULL)
//       {
//         // elog(NOTICE, "MaterialCustomNext ---------- 7");
//         ExecSetSlotDescriptor(CacheNode->tmpslot, firstSlot->tts_tupleDescriptor);
//         // elog(NOTICE, "MaterialCustomNext ---------- 8");
//         ExecStoreAllNullTuple(CacheNode->tmpslot);   
//       }

//       ListCell * l;
//       foreach(l, CacheNode->hash_needed)
//       {
//         // elog(NOTICE, "MaterialCustomNext ---------- 9");
//         int     varNumber = lfirst_int(l) - 1;

//         CacheNode->tmpslot->tts_values[varNumber] = slot->tts_values[varNumber];
//         CacheNode->tmpslot->tts_isnull[varNumber] = slot->tts_isnull[varNumber];
//         // elog(NOTICE, "MaterialCustomNext ---------- 10");
//       }

//       // elog(NOTICE, "MaterialCustomNext ---------- 11");
//       // tmpcontext->ecxt_outertuple = tmpSlot;
//       /* Find or build hashtable entry for this outerSlot and modify it to be qualify */
//       bool isnew = false;
//       entry = lookup_hash_entry(CacheNode, CacheNode->tmpslot, &isnew);

//       CacheNode->childResultinCache = true;

//       // elog(NOTICE, "MaterialCustomNext ---------- 12");
//       if(isnew && entry != NULL) // is added
//       {
//         // elog(NOTICE, "MaterialCustomNext ---------- 13, Entry is added ");
//         printf("Material: isnew - %lu ", CacheNode->tmpslot->tts_values[0]);
//         /* by default, the tuple is not qualify unless it will set by the cache node*/
//         entry->DoesQualify = false;
//         CacheNode->custom_tuple_returned = NULL;
//         CacheNode->custom_tuple_returned = ExecStoreMinimalTuple(entry->shared.firstTuple,
//               CacheNode->tmpslot,
//               false);
//         CacheNode->entryFromChild = entry;
//         printf(" - custom_tuple_returned - %lu \n", CacheNode->custom_tuple_returned->tts_values[0]);
//         // elog(NOTICE, "MaterialCustomNext ---------- 14");
//         return slot;
//       }
//       else
//       {
//         // elog(NOTICE, "MaterialCustomNext ---------- 15, Entry already exists");

//         /* it's already exist in the hash */
//         if(entry->DoesQualify)
//         {
//           // elog(NOTICE, "MaterialCustomNext ---------- 16, Entry Qualify");
//           printf("Material: NOT new BUT Qualify - %lu ", CacheNode->tmpslot->tts_values[0]);

//           CacheNode->custom_tuple_returned = NULL;
//           CacheNode->custom_tuple_returned = ExecStoreMinimalTuple(entry->shared.firstTuple,
//               CacheNode->tmpslot,
//               false);
//           CacheNode->entryFromChild = entry;
//           printf(" -custom_tuple_returned- %lu \n", CacheNode->custom_tuple_returned->tts_values[0]);
//           // elog(NOTICE, "MaterialCustomNext ---------- 17");
//           return NULL;
//         }
//         // else 
//         /* here go and get the next tuple */
//         printf("Material: NOT new AND NOT Qualify - %lu \n", CacheNode->tmpslot->tts_values[0]);
//       }
//       // elog(NOTICE, "MaterialCustomNext ---------- 18");
//     }
//     else /* no more tuples from the child, then return */
//       return slot;
//     //============================================
//   }
//   // elog(NOTICE, "MaterialCustomNext ---------- finish");
//   return slot;
// }
static TupleTableSlot * MaterialGenericCustomNext(CustomScanState *node)
{
  TupleTableSlot *slot = NULL;
  MaterialGenericState * tmp = (MaterialGenericState * ) node; 
  CacheGenericState * CacheNode = (CacheGenericState *) tmp->CacheCSS;
  // CacheHashEntry entry = NULL;
  TupleTableSlot *firstSlot ;
  // elog(NOTICE, "MaterialCustomNext ---------- start");

  while(true)
  {
    slot = NULL;
    slot = ExecProcNode((PlanState*)linitial( node->custom_ps));
    
    // elog(NOTICE, "MaterialCustomNext ---------- 1");
    //============================================
    // elog(NOTICE, "MaterialCustomNext ---------- Cache: es_processed : %lu ", CacheNode->css.ss.ps.state->es_processed);
    // elog(NOTICE, "MaterialCustomNext ---------- Material: es_processed : %lu ", tmp->css.ss.ps.state->es_processed);

    // initialization 
    CacheNode->entryFromChild = NULL;
    CacheNode->childResultinCache = false;
    if(CacheNode->tmpslot != NULL && CacheNode->tmpslot->tts_tupleDescriptor != NULL)
      ExecStoreAllNullTuple(CacheNode->tmpslot);

    if(!TupIsNull(slot))
    {
      // DEBUG
      // if(slot->tts_values[0] == 1237)
      //   elog(NOTICE, "MaterialCustomNext ------- DEBUG DEBUG DEBUG");
      // DEBUG

      // elog(NOTICE, "MaterialCustomNext ---------- 2");
      
      
      firstSlot = CacheNode->css.ss.ss_ScanTupleSlot;

      // elog(NOTICE, "MaterialCustomNext ---------- 3");
      /* build a new slot with the same desc as cache node child */
      if(CacheNode->hashslot->tts_tupleDescriptor == NULL && firstSlot->tts_tupleDescriptor != NULL)
      { 
        // elog(NOTICE, "MaterialCustomNext ---------- 4");
        ExecSetSlotDescriptor(CacheNode->hashslot, firstSlot->tts_tupleDescriptor);
        // elog(NOTICE, "MaterialCustomNext ---------- 5");
        ExecStoreAllNullTuple(CacheNode->hashslot);
        // elog(NOTICE, "MaterialCustomNext ---------- 6");
      }

      if(CacheNode->tmpslot->tts_tupleDescriptor == NULL)
      {
        // elog(NOTICE, "MaterialCustomNext ---------- 7");
        ExecSetSlotDescriptor(CacheNode->tmpslot, firstSlot->tts_tupleDescriptor);
        // elog(NOTICE, "MaterialCustomNext ---------- 8");
        ExecStoreAllNullTuple(CacheNode->tmpslot);   
      }

      ListCell * l;
      foreach(l, CacheNode->hash_needed)
      {
        // elog(NOTICE, "MaterialCustomNext ---------- 9");
        int     varNumber = lfirst_int(l) - 1;

        CacheNode->tmpslot->tts_values[varNumber] = slot->tts_values[varNumber];
        CacheNode->tmpslot->tts_isnull[varNumber] = slot->tts_isnull[varNumber];
        // elog(NOTICE, "MaterialCustomNext ---------- 10");
      }

      /* Find or build hashtable entry for this outerSlot and modify it to be qualify */
      bool isnew = false;
      // entry = NULL;
      CacheHashEntry entry = NULL;
      entry = lookup_hash_entry(CacheNode, CacheNode->tmpslot, &isnew);

      CacheNode->childResultinCache = true;

      // elog(NOTICE, "MaterialCustomNext ---------- 12");
      if(isnew && entry != NULL) // is added
      {
        // elog(NOTICE, "MaterialCustomNext ---------- 13, Entry is added ");
        // printf("Material: isnew - %lu \n", CacheNode->tmpslot->tts_values[0]);
        
        entry->DoesQualify = false;
        entry->undefinedstatus = true;

        CacheNode->custom_tuple_returned = NULL;
        // CacheNode->custom_tuple_returned = ExecStoreMinimalTuple(entry->shared.firstTuple,
        //       CacheNode->tmpslot,
        //       false);


        //DEBUG
        // ListCell * l;
        // foreach(l , CacheNode->udefinedtuplesKeys)
        // {
        //   // CacheHashEntry en = (CacheHashEntry) lfirst(l);
        //   TupleTableSlot * en = (TupleTableSlot *) lfirst(l);
        //   printf("Material: undefined entry BEFORE - %p  - tmpslot : %p \n", en, CacheNode->tmpslot);
        // }
        //DEBUG
        MemoryContext oldCtx;
        oldCtx = MemoryContextSwitchTo(CacheNode->tupContexts);
        TupleTableSlot * tmp =  MakeSingleTupleTableSlot(CacheNode->tmpslot->tts_tupleDescriptor);
        tmp->tts_mcxt = CacheNode->tupContexts;
        tmp = ExecCopySlot(tmp , CacheNode->tmpslot );
        CacheNode->udefinedtuplesKeys = lappend(CacheNode->udefinedtuplesKeys,tmp); // ??? I may need to define the entry each time in the loop so the pointed data will not be gone when et entry = NULL

        //DEBUG
        // ListCell * l;
        // foreach(l , CacheNode->udefinedtuplesKeys)
        // {
        //   // CacheHashEntry en = (CacheHashEntry) lfirst(l);
        //   TupleTableSlot * en = (TupleTableSlot *) lfirst(l);
        //   printf("Material: undefined entry AFTER - %p \n", en);
        // }
        //DEBUG

        // printf(" - custom_tuple_returned - %lu \n", CacheNode->custom_tuple_returned->tts_values[0]);
        // elog(NOTICE, "MaterialCustomNext ---------- 14");
        return slot;
      }
      else
      {
        // elog(NOTICE, "MaterialCustomNext ---------- 15, Entry already exists");

        /* it's already exist in the hash */
        if(entry->DoesQualify && !entry->undefinedstatus)
        {
          // elog(NOTICE, "MaterialCustomNext ---------- 16, Entry Qualify: %lu", CacheNode->tmpslot->tts_values[0]);
          // printf("Material: NOT new BUT Qualify - %lu \n", CacheNode->tmpslot->tts_values[0]);

          CacheNode->custom_tuple_returned = NULL;


          // TupleTableSlot * t = ExecStoreMinimalTuple(entry->shared.firstTuple,
          //                           CacheNode->tmpslot,
          //                           false);
          // TupleTableSlot * t = slot;
            //DEBUG
            // ListCell * ll;
            // foreach(ll, CacheNode->returnedTuples)
            // {
            //   TupleTableSlot * tt = (TupleTableSlot * ) lfirst(ll);
            //   {
            //     // CacheHashEntry entryt = (CacheHashEntry) lfirst(ll);
            //      // tt = entryt->tuple; 
            //   }
            //   if(tt != NULL)
            //     printf("Material ----------, BEFORE tt = %lu\n", tt->tts_values[0]);
            // }
            //DEBUG

          // elog(NOTICE, "MaterialCustomNext ---------- 17");
          
          MemoryContext oldCtx;
          oldCtx = MemoryContextSwitchTo(CacheNode->tupContexts);
          TupleTableSlot * tmp =  MakeSingleTupleTableSlot(entry->tuple->tts_tupleDescriptor);
          tmp->tts_mcxt = CacheNode->tupContexts;
          tmp = ExecCopySlot(tmp , entry->tuple );

          // CacheHashEntryData * hashentry = palloc(sizeof(CacheHashEntryData));

          // hashentry->shared.firstTuple = (MinimalTuple) palloc(entry->shared.firstTuple->t_len);
          // memcpy(hashentry->shared.firstTuple, entry->shared.firstTuple, entry->shared.firstTuple->t_len);

          // hashentry->shared.firstTuple = ExecCopySlotMinimalTuple(CacheNode->tmpslot);
          // hashentry->DoesQualify = entry->DoesQualify;
          // hashentry->undefinedstatus = entry->undefinedstatus;
          // hashentry->tuple = entry->tuple;
          // entry = NULL;
          CacheNode->returnedTuples = lappend(CacheNode->returnedTuples , tmp);
          
          MemoryContextSwitchTo(oldCtx);
          // //DEBUG
          //   // ListCell * ll;
          //   foreach(ll, CacheNode->returnedTuples)
          //   {
          //     TupleTableSlot * tt = (TupleTableSlot * ) lfirst(ll);
          //     {
          //       // CacheHashEntry entryt = (CacheHashEntry) lfirst(ll);
          //        // tt = entryt->tuple; 
          //     }
          //     if(tt != NULL)
          //       printf("Material ----------, AFTER tt = %lu\n", tt->tts_values[0]);
          //   }
            //DEBUG

          // CacheNode->custom_tuple_returned = ExecStoreMinimalTuple(entry->shared.firstTuple,
          //     CacheNode->tmpslot,
          //     false);
          // CacheNode->entryFromChild = entry;
          // printf(" -custom_tuple_returned- %lu \n", CacheNode->custom_tuple_returned->tts_values[0]);
          // elog(NOTICE, "MaterialCustomNext ---------- 17");
          // return NULL;
        }
        // else 
        /* here go and get the next tuple */
        // printf("Material: NOT new AND NOT Qualify - %lu \n", CacheNode->tmpslot->tts_values[0]);
      }
      // elog(NOTICE, "MaterialCustomNext ---------- 18");
    }
    else /* no more tuples from the child, then return */
      return slot;
    //============================================
  }
  // elog(NOTICE, "MaterialCustomNext ---------- finish");
  return slot;
}

static bool MaterialGenericCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true; 
}

void End_MaterialGenericCustomScan (CustomScanState *node)
{
  // elog(NOTICE, "End_MaterialCustomScan -------- start");
  
  /*
  * Free the exprcontext
  */
  ExecFreeExprContext(&node->ss.ps);

  /*
  * clean out the upper tuple table
  */
  ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
  * close down subquery
  */

  //End child plans
  // if(list_length(node->custom_ps) == 1)
    ExecEndNode((PlanState*)linitial(node->custom_ps));
  // else //TODO
  // elog(NOTICE, "End_MaterialCustomScan -------- finish");
}

void ReScan_MaterialGenericCustomScan (CustomScanState *node)
{

  // elog(NOTICE, "ReScan_MaterialCustomScan -------- start");
  ExecScanReScan(&node->ss);

  /*
  * ExecReScan doesn't know about my subplan, so I have to do
  * changed-parameter signaling myself.  This is just as well, because the
  * subplan has its own memory context in which its chgParam state lives.
  */
  PlanState * childplanstate =  (PlanState *)linitial( node->custom_ps);
  if (node->ss.ps.chgParam != NULL)
    UpdateChangedParamSet(childplanstate, node->ss.ps.chgParam);

  /*
  * if chgParam of subnode is not null then plan will be re-scanned by
  * first ExecProcNode.
  */
  if (childplanstate->chgParam == NULL)
    ExecReScan(childplanstate);

  // elog(NOTICE, "ReScan_MaterialCustomScan -------- Finish");
}


static void Explain_MaterialGenericCustomScan(CustomScanState *node, List *ancestors, ExplainState *es)
{

}


Path * walkPath( Path * pathRoot )
{
  Path * customPathNode = NULL;
  if (pathRoot == NULL)
    return NULL;

  customPathNode = NULL;
  
  switch (pathRoot->pathtype)
  {
    case T_SeqScan:
    case T_SampleScan:
    case T_IndexScan:
    case T_IndexOnlyScan:
    case T_BitmapHeapScan:
    case T_TidScan:
      break;

    case T_SubqueryScan:
      {
      SubqueryScanPath * p = (SubqueryScanPath *) pathRoot;
      customPathNode = walkPath(p->subpath);
      }
      break;
    case T_FunctionScan:
    case T_ValuesScan:
    case T_CteScan:
    case T_WorkTableScan:
    case T_ForeignScan:
    case T_CustomScan:
        customPathNode = pathRoot;
      break;
    case T_HashJoin:
    case T_MergeJoin:
    case T_NestLoop:
      {
        JoinPath * p = (JoinPath *) pathRoot;
        customPathNode = walkPath(p->outerjoinpath);
        if(customPathNode == NULL)
          customPathNode = walkPath(p->innerjoinpath);
      }
      break;
    case T_Append:
    case T_MergeAppend:
    case T_Result:
    case T_Material:
    case T_Unique:
    case T_Gather:
      break;
    case T_Sort:
      {
        SortPath * p = (SortPath *) pathRoot;
        customPathNode = walkPath(p->subpath);
    }
      break;
    case T_Group:
      {
        GroupPath * p = (GroupPath *) pathRoot;
        customPathNode = walkPath(p->subpath);
      }
      break;
    case T_Agg:
      {
        if (IsA(pathRoot, GroupingSetsPath))
        {
          GroupingSetsPath * p = (GroupingSetsPath *) pathRoot;
          customPathNode = walkPath(p->subpath);
        }                
        else if (IsA(pathRoot, AggPath))
        {
           AggPath * p = (AggPath *) pathRoot;
           customPathNode = walkPath(p->subpath);
        }
      }
      break;
    case T_WindowAgg:
      {
        WindowAggPath * p = (WindowAggPath *) pathRoot;
        customPathNode = walkPath(p->subpath);
      }
      break;
    case T_SetOp:
    case T_RecursiveUnion:
    case T_LockRows:
    case T_ModifyTable:
    case T_Limit:
      break;
    default:
      elog(ERROR, "unrecognized node type: %d",
         (int) pathRoot->pathtype);
      customPathNode = NULL;    /* keep compiler quiet */
      break;
  }
   
  
  return customPathNode;
}

void walkPlan( Plan * planRoot, List ** custom_plans )
{
  // custom_plans = NIL;
  if (planRoot == NULL)
    return ;

  // customPathNode = NULL;
  
  switch (nodeTag(planRoot) )
  {
    case T_SeqScan:
    case T_SampleScan:
    case T_IndexScan:
    case T_IndexOnlyScan:
    case T_BitmapHeapScan:
    case T_TidScan:
      break;

    case T_SubqueryScan:
      {
        SubqueryScan * p = (SubqueryScan *) planRoot;
        walkPlan( p->subplan , custom_plans);
      }
      break;
    case T_FunctionScan:
    case T_ValuesScan:
    case T_CteScan:
    case T_WorkTableScan:
    case T_ForeignScan:
    
    case T_CustomScan:
      {
        /* this will push child custom Scan above the parent Custom Scan*/
        *custom_plans = lcons(planRoot , *custom_plans);

        /* this will push child custom Scan under the parent Custom Scan*/
        // *custom_plans = lappend( *custom_plans , planRoot);

        CustomScan * p = (CustomScan *) planRoot;
        
        if(p->custom_plans)
        {
          ListCell * cell ;
          foreach(cell , p->custom_plans)
          {
            walkPlan( (Plan *) lfirst(cell) , custom_plans);  
          }
        }

        // if(p->custom_private )
        // {
        //   ListCell * cell ;
        //   foreach(cell , p->custom_private)
        //   {
        //     Plan * tmp = (Plan *) lfirst(cell);
        //     if(IsA(tmp , Plan))
        //       walkPlan( tmp , custom_plans);  
        //   }
        // }

        // here assume only the child plan will be the first elemnt in the cutom_private list
        if(p->custom_private )
        {
          walkPlan( (Plan *) linitial(p->custom_private) , custom_plans);  
        }
      }
      break;
    
    case T_HashJoin:
    case T_MergeJoin:
    case T_NestLoop:
      {
        Join * p = (Join *) planRoot;
        walkPlan( innerPlan(p) , custom_plans);
        walkPlan( outerPlan(p) , custom_plans);
      }
      break;
    case T_Append:
    case T_MergeAppend:
    case T_Result:
    case T_Material:
    case T_Unique:
    case T_Gather:
      break;
    case T_Sort:
      {
        Sort * p = (Sort *) planRoot;
        walkPlan( innerPlan(p) , custom_plans);
        walkPlan( outerPlan(p) , custom_plans);
    }
      break;
    case T_Group:
      {
        Group * p = (Group *) planRoot;
        walkPlan( innerPlan(p) , custom_plans);
        walkPlan( outerPlan(p) , custom_plans);
      }
      break;
    case T_Agg:
      {
        Agg * p = (Agg *) planRoot;
        walkPlan( innerPlan(p) , custom_plans);
        walkPlan( outerPlan(p) , custom_plans);
      }
      break;
    case T_WindowAgg:
      {
        WindowAgg * p = (WindowAgg *) planRoot;
        walkPlan( innerPlan(p) , custom_plans);
        walkPlan( outerPlan(p) , custom_plans);
      }
      break;
    case T_SetOp:
    case T_RecursiveUnion:
    case T_LockRows:
    case T_ModifyTable:
    case T_Limit:
      break;
    default:
      elog(WARNING, "unrecognized node type: %d",
         (int) nodeTag(planRoot) );
      // customPathNode = NULL;    /* keep compiler quiet */
      break;
  }
   
  
  // return customPathNode;
  return;
}

void walkPlanState( PlanState * planRoot, List ** custom_plans )
{
  // custom_plans = NIL;
  if (planRoot == NULL)
    return ;

  
  
  switch (nodeTag(planRoot) )
  {
      /*
       * control nodes
       */
    case T_ResultState:
      // result = ExecResult((ResultState *) node);
      break;

    case T_ModifyTableState:
      // result = ExecModifyTable((ModifyTableState *) node);
      break;

    case T_AppendState:
      // result = ExecAppend((AppendState *) node);
      break;

    case T_MergeAppendState:
      // result = ExecMergeAppend((MergeAppendState *) node);
      break;

    case T_RecursiveUnionState:
      // result = ExecRecursiveUnion((RecursiveUnionState *) node);
      break;

      /* BitmapAndState does not yield tuples */

      /* BitmapOrState does not yield tuples */

      /*
       * scan nodes
       */
    case T_SeqScanState:
      // result = ExecSeqScan((SeqScanState *) node);
      break;

    case T_SampleScanState:
      // result = ExecSampleScan((SampleScanState *) node);
      break;

    case T_IndexScanState:
      // result = ExecIndexScan((IndexScanState *) node);
      break;

    case T_IndexOnlyScanState:
      // result = ExecIndexOnlyScan((IndexOnlyScanState *) node);
      break;

      /* BitmapIndexScanState does not yield tuples */

    case T_BitmapHeapScanState:
      // result = ExecBitmapHeapScan((BitmapHeapScanState *) node);
      break;

    case T_TidScanState:
      // result = ExecTidScan((TidScanState *) node);
      break;

    case T_SubqueryScanState:
      {
        SubqueryScanState * p = (SubqueryScanState * )planRoot; 
        walkPlanState( p->subplan , custom_plans);
      }
      break;

    case T_FunctionScanState:
      // result = ExecFunctionScan((FunctionScanState *) node);
      break;

    case T_ValuesScanState:
      // result = ExecValuesScan((ValuesScanState *) node);
      break;

    case T_CteScanState:
      // result = ExecCteScan((CteScanState *) node);
      break;

    case T_WorkTableScanState:
      // result = ExecWorkTableScan((WorkTableScanState *) node);
      break;

    case T_ForeignScanState:
      // result = ExecForeignScan((ForeignScanState *) node);
      break;

    case T_CustomScanState:
      {
        *custom_plans = lcons(planRoot , *custom_plans);

        /* this will push child custom Scan under the parent Custom Scan*/
        // *custom_plans = lappend( *custom_plans , planRoot);

        CustomScanState * p = (CustomScanState *) planRoot;
        
        if(p->custom_ps)
        {
          ListCell * cell ;
          foreach(cell , p->custom_ps)
          {
            walkPlanState( (PlanState *) lfirst(cell) , custom_plans);  
          }
        }
      }
      break;

      /*
       * join nodes
       */
    case T_NestLoopState:
      {
      NestLoopState * p = (NestLoopState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_MergeJoinState:
      {
      MergeJoinState * p = (MergeJoinState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_HashJoinState:
      {
      HashJoinState * p = (HashJoinState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

      /*
       * materialization nodes
       */
    case T_MaterialState:
      // result = ExecMaterial((MaterialState *) node);
      break;

    case T_SortState:
      {
      SortState * p = (SortState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_GroupState:
      {
      GroupState * p = (GroupState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_AggState:
      {
      AggState * p = (AggState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_WindowAggState:
      {
      WindowAggState * p = (WindowAggState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_UniqueState:
      // result = ExecUnique((UniqueState *) node);
      break;

    case T_GatherState:
      {
      GatherState * p = (GatherState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_HashState:
      {
      HashState * p = (HashState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    case T_SetOpState:
      // result = ExecSetOp((SetOpState *) node);
      break;

    case T_LockRowsState:
      // result = ExecLockRows((LockRowsState *) node);
      break;

    case T_LimitState:
      {
      LimitState * p = (LimitState *) planRoot;
      walkPlanState(innerPlanState(p) , custom_plans);
      walkPlanState(outerPlanState(p) , custom_plans);
      }
      break;

    default:
      elog(ERROR, "unrecognized node type: %d", (int) nodeTag(planRoot));
      // result = NULL;
      break;
  } 
  
  
  return;
}
// Plan * walk( Plan *plan)
// {
//   Plan * subqueryPlan = NULL;
//   if (plan == NULL)
//     return NULL;

//   bool found = false;
//   subqueryPlan = plan;
//   while(!found)
//   {
//     switch (nodeTag(subqueryPlan))
//     {
//       // case T_SubqueryScan:
//       //   /* Needs special treatment, see comments below */
//       //   return set_subqueryscan_references(root,
//       //                      (SubqueryScan *) plan,
//       //                      rtoffset);
      
//       case T_CustomScan:
//         return subqueryPlan;
//         break;

//       case T_NestLoop:
//         {
//           NestLoop * nestplan = (NestLoop * ) subqueryPlan;
//           subqueryPlan = nestplan->join.plan.righttree;
//         }
//         break;

      
//       default:
//         elog(ERROR, "unrecognized node type: %d",
//            (int) nodeTag(plan));
//         break;
//     }
//   }
//   return subqueryPlan;
// }
 /* 
      this one to be pushed above the inner path of the Join Node 
      So, current Path should be the SubaueryScan path (inner path)
      and the child path should be the subpath
 */
CustomPath* create_CacheGenericCustom_path(Path* currentPath , Path* childPath)
{
  // elog(NOTICE, "create_CacheGenericCustom_path ----------- start , relid= %d " , currentPath->parent->relid);
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  
  RelOptInfo * rel = currentPath->parent;

  Assert(rel->rtekind == RTE_SUBQUERY) ;

  pathnode->path.parent = rel;
  
  // pathnode->custom_paths = lappend(pathnode->custom_paths, childPath);    
  pathnode->path.param_info = currentPath->param_info; 

  pathnode->path.pathtarget = currentPath->pathtarget;
  pathnode->path.parallel_aware = false;
  pathnode->path.parallel_safe = currentPath->parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
  pathnode->path.parallel_workers = currentPath->parallel_workers;
  pathnode->path.pathkeys = currentPath->pathkeys; // ?? or should I put the child pathkeys
  
  /* compute the cost */
  pathnode->path.rows = currentPath->rows;     /* estimated number of result tuples */
  pathnode->path.startup_cost = currentPath->startup_cost;  //cost expended before fetching any tuples 
  pathnode->path.total_cost = currentPath->total_cost;   /* TODO: need to estimate the cost ?? */

  /* set the value of K (Limit Value) */
  SubqueryScanPath * currSubqueryPath = (SubqueryScanPath *) currentPath;
  Assert(IsA(currSubqueryPath, SubqueryScanPath));
  // Assert(IsA(currSubqueryPath->subpath, LimitPath));
  
  
  /* TODO: I need to put the child path not the subpath of the subquery path */
  pathnode->custom_private =lappend(pathnode->custom_private , ((SubqueryScanPath *)currentPath)->subpath); 
  
  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "CacheGenericCustompath";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_CacheGenericCustomPath;
  pathnode->methods = methods;
  
  

  //DEBUG
  
  // printf("\ncreate_CacheGenericCustom_path ============ currentPath: \n");
  // pprint(currentPath);

  // printf("create_CacheGenericCustom_path ============= CustomPath:\n");
  // pprint(pathnode);

  // printf("create_CacheGenericCustom_path ============= subroot:\n");
  // pprint(rel->subroot);

  //DEBUG

  return pathnode;
}



/* Plan for replace_cacheCustomScan_path */
static Plan * Plan_CacheGenericCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  // elog(NOTICE, "Plan_CacheGenericCustomPath ----------- start , relid= %d " , rel->relid);
  //DEBUG
  // printf("\nPlan_CacheGenericCustomPath ----------- rel:\n");
  // pprint(rel);
  // printf("\nPlan_CacheGenericCustomPath ----------- best_path:\n");
  // pprint(best_path);
  // printf("\nPlan_CacheGenericCustomPath ----------- tlist:\n");
  // pprint(tlist);
  // printf("\nPlan_CacheGenericCustomPath ----------- clauses:\n");
  // pprint(clauses);
  // printf("\nPlan_CacheGenericCustomPath ----------- custom_plans:\n");
  // pprint(custom_plans);
  //DEBUG

  /* STEP 1: */
 
  // rel = best_path->path.parent;
  Index   scan_relid = rel->relid;
  Plan     *subplan;

  /* it should be a subquery base rel... */
  Assert(scan_relid > 0);
  Assert(rel->rtekind == RTE_SUBQUERY);
  Assert(rel->subroot != NULL);
  
  subplan = create_plan(rel->subroot, (Path *)linitial (best_path->custom_private) );

  // printf("\nPlan_CacheGenericCustomPath ----------- rel->subroot:\n");
  // pprint(rel->subroot);
  
  
  /* STEP 2: */
  clauses = extract_actual_clauses(clauses, false);

  /* STEP 3: */
  if (best_path->path.param_info)
  {
    // clauses = (List *)
    //   replace_nestloop_params(root, (Node *) clauses);
    process_subquery_nestloop_params(root,
                     rel->subplan_params);
  }

  // //DEBUG
  //  elog(NOTICE, "Plan_CacheGenericCustomPath ----------- 1 , set_plan_references : " );
  // subplan = set_plan_references(rel->subroot , subplan);
  // elog(NOTICE, "Plan_CacheGenericCustomPath ----------- 2 , set_plan_references : " );
  // //DEBUG

  /* STEP 4: */
  CustomScan * customscanNode; 
  
  /* Make the customScan plan node */
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_scan_tlist = tlist;  
  // customscanNode->custom_plans = lappend(customscanNode->custom_plans , subplan);
  customscanNode->custom_private = lappend(customscanNode->custom_private , subplan);
  customscanNode->custom_private = lappend(customscanNode->custom_private , rel->subroot);
  // customscanNode->custom_exprs = clauses;
  customscanNode->flags = best_path->flags;

  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
 
  plan->lefttree = NULL;//(Plan *)linitial(custom_plans); //custom_plans; ???
  plan->righttree = NULL; // ???

  //DEBUG
  // customscanNode->custom_relids = bms_add_member(customscanNode->custom_relids, scan_relid);
  //DEBUG
  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "PlanCacheGenericCustomPath";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_CacheGenericCustomScan_state;
  customscanNode->methods = methods;

  // printf("\nPlan_CacheGenericCustomPath ----------- customScanNode:\n");
  // pprint(customscanNode);
  // elog(NOTICE, "Plan_CacheGenericCustomPath ----------- FINISH");

  return (Plan *) customscanNode;
}





Node *create_CacheGenericCustomScan_state(CustomScan *node)
{
  // elog(NOTICE, "create_CacheGenericCustomScan_state ------------ start");


  CacheGenericState * CacheGenericNode =  palloc(sizeof(CacheGenericState));
  // CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
  // CustomScanState
  /* TODO: new data Structure that contains the priority Queue to hold materialzed points */
  
  CustomScanState * MaterialCustomScanNode = makeNode(CustomScanState); //palloc(sizeof(MaterialCustomScanNode));
  // CustomScanState * MaterialCustomScanNode = malloc(sizeof(MaterialCustomScanNode));
  MaterialCustomScanNode->ss.ps.type = T_CustomScanState;
  

  CacheGenericNode->css = *MaterialCustomScanNode;


  /*
   * Set the basic state structure 
   */
  MaterialCustomScanNode->custom_ps = NIL;
  
  MaterialCustomScanNode->custom_ps = lappend(MaterialCustomScanNode->custom_ps, linitial (node->custom_plans) );
  
  MaterialCustomScanNode->pscan_len = 0;  
  // printf("\ncreate_CacheGenericCustomScan_state ----------- MaterialCustomScanNode->custom_ps:\n");
  // pprint(MaterialCustomScanNode->custom_ps);
 
  
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  
  MaterialCustomScanNode->ss.ps.instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.worker_instrument = NULL; // ???
  MaterialCustomScanNode->ss.ps.chgParam = NULL; // ???
  MaterialCustomScanNode->ss.ss_currentRelation = NULL;
  // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  
  MaterialCustomScanNode->ss.ps.lefttree = NULL;
  MaterialCustomScanNode->ss.ps.righttree = NULL; 
  MaterialCustomScanNode->ss.ps.initPlan = NULL; //?????
  MaterialCustomScanNode->ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  // methods = palloc(sizeof( methods));
  // char * name = palloc(sizeof(char) * 50);
  methods = malloc(sizeof( *methods));
  char * name = malloc(sizeof(char)*50);
  name = "CacheGenericCustomScanstate";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_CacheGenericCustomScan;
  methods->ExecCustomScan = Exec_CacheGenericCustomScan;
  methods->EndCustomScan = End_CacheGenericCustomScan;
  methods->ReScanCustomScan = ReScan_CacheGenericCustomScan;
  methods->ExplainCustomScan = Explain_CacheGenericCustomScan;
  
  MaterialCustomScanNode->methods = methods;

  // if(MaterialCustomScanNode->methods->BeginCustomScan != NULL)
  // {
  //   elog(NOTICE, "create_CacheGenericCustomScan_state ------- call BeginCustomScan");
  //   // MaterialCustomScanNode->methods->BeginCustomScan(MaterialCustomScanNode ,
  //   //                                                  NULL, 0);
  // }

  //=================================================
  /*
   * TODO: intialize the hash table 
   */

  CacheGenericNode->hashtable = NULL;
  /* rest of initialization will be in begin_cacheGenericCustomScan () */

  // elog(NOTICE, "create_CacheGenericCustomScan_state ------------ FINISH");  
  return (Node *) MaterialCustomScanNode;
}



void Begin_CacheGenericCustomScan(CustomScanState *node, EState *estate, int eflags)
{
  // elog(NOTICE, "Begin_CacheGenericCustomScan ------------ start");

  PlanState * childplanstate = NULL;
  Plan * childPlan = NULL;
  CustomScanState * childCustomPS = NULL;
  CacheGenericState * CacheGenericNode = (CacheGenericState * ) node;

  /* initilaize the custom_exprs */
  CustomScan * plan = (CustomScan *)node->ss.ps.plan;

  childPlan = (Plan *)linitial (plan->custom_plans);
  childplanstate = ExecInitNode(childPlan, estate, eflags);

  /* initialize children plan state */
  if(childplanstate)
  {
    node->custom_ps = NIL;
    node->custom_ps = lappend(node->custom_ps , childplanstate) ;

    /*
    * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
    */
    ExecAssignScanType(&node->ss,ExecGetResultType(childplanstate));

    //====================================================
    /* this step to make the child plan points to the parent plan
       so the child plan node can check the index hash table for existing tuples 
    */

    // walk to find the planState of the child custom node
    List * custom_plans = NIL;
    walkPlanState( (PlanState *)node, &custom_plans );

    /* for current assumption, only one custom node child for cache custom node */
    childCustomPS = (CustomScanState *) linitial(custom_plans); 
    if (childCustomPS)
    {
      /* child custom node has a pointer to the parent custom node in its private list */
      
      /* cast to the defined larger structure for the childe node */
      MaterialGenericState * tmp = (MaterialGenericState * ) childCustomPS; 
      tmp->CacheCSS =  CacheGenericNode;
    }
    //====================================================
  }
  
  //=================================================
  /*
   * TODO: intialize the hash table 
   */

  
  
  /* initialize tmp context for short term computations */
  ExecAssignExprContext(estate, &CacheGenericNode->css.ss.ps);
  CacheGenericNode->tmpcontext = CacheGenericNode->css.ss.ps.ps_ExprContext;

  /*  intialize the econtext for long term data */
  ExecAssignExprContext(estate, &CacheGenericNode->css.ss.ps);
  CacheGenericNode->econtexts = CacheGenericNode->css.ss.ps.ps_ExprContext;

  CacheGenericNode->tupContexts = AllocSetContextCreate(CurrentMemoryContext,
                    "CacheGenericState memory",
                    ALLOCSET_DEFAULT_SIZES);

  // ExecAssignExprContext(estate, &CacheGenericNode->css.ss.ps);
  //  = CacheGenericNode->css.ss.ps.ps_ExprContext;

  ExecAssignExprContext(estate, &CacheGenericNode->css.ss.ps);

  CacheGenericNode->hashslot = ExecInitExtraTupleSlot(estate);
  CacheGenericNode->tmpslot = ExecInitExtraTupleSlot(estate);

  /*
   *  TODO: here I have assumed that I'll hash based in the first attribute in the 
   *         child targetlist (which is should be of type 20 , primary key, like cutomerKey ) 
   */
  CacheGenericNode->numCols = 1;
  /* equality operators to compare with */
  CacheGenericNode->Operators = (Oid *) palloc(sizeof(Oid) * CacheGenericNode->numCols);
  CacheGenericNode->Operators[0] = 410; /* this is '=' operator */
  
  CacheGenericNode->grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * CacheGenericNode->numCols);    /* their indexes in the target list */
  CacheGenericNode->grpColIdx[0] = 1; // the custkey in the targetlist resno = 1  
  
  execTuplesHashPrepare(CacheGenericNode->numCols,
                CacheGenericNode->Operators,
                &CacheGenericNode->eqfunctions,
                &CacheGenericNode->hashfunctions);


  CacheGenericNode->HashTableSize = 1;

  if( IsA ((PlanState * )linitial( childCustomPS->custom_ps) , LimitState) )
  {
    LimitState * limitps =  (LimitState * )linitial( childCustomPS->custom_ps);
    if(IsA (outerPlanState(limitps) , IndexScanState))
    {
      IndexScanState * indexps = (IndexScanState * )outerPlanState(limitps) ;
      CacheGenericNode->HashTableSize = indexps->ss.ps.plan->plan_rows;
    }

  }
  
  // elog(NOTICE, "Begin_CacheGenericCustomScan ------------ 3, HashTableSize = %ld", CacheGenericNode->HashTableSize);

  build_hash_table(CacheGenericNode);
  
  // aggstate->table_filled = false;
  /* Compute the columns we actually need to hash on 
      For Now: I'll use the only col I assumed we are hashing on, the custkey 
  */

  CacheGenericNode->hash_needed = NIL;  //find_hash_columns(aggstate);
  CacheGenericNode->hash_needed = lcons_int(CacheGenericNode->grpColIdx[0], CacheGenericNode->hash_needed );  //find_hash_columns(aggstate);

  CacheGenericNode->childResultinCache = false;
  CacheGenericNode->udefinedtuplesKeys = NIL;
  CacheGenericNode->returnedTuples = NIL;
  //=================================================

  // elog(NOTICE, "Begin_CacheGenericCustomScan ------------ FINISH");
}



TupleTableSlot *  Exec_CacheGenericCustomScan (CustomScanState *node)
{ 
  // elog(NOTICE, "Exec_CacheGenericCustomScan -------- start" );
  
  TupleTableSlot * result = NULL;

  result =  ExecScan(&(node->ss),
          (ExecScanAccessMtd) CacheGenericCustomNext,
          (ExecScanRecheckMtd) CacheGenericCustomRecheck);

  return result;
}

// version 1, not working correctly
// static TupleTableSlot * CacheGenericCustomNext(CustomScanState *node)
// {
//   TupleTableSlot *outerslot = NULL;
//   CacheGenericState * CacheNode = (CacheGenericState * ) node;

//   // elog(NOTICE, "CacheCustomNext ---------- start");

//   /* some initializing before calling the child */
//   CacheNode->childResultinCache = false;
//   CacheNode->NeedModification = false;
//   CacheNode->custom_tuple_returned = NULL;
//   CacheNode->entryFromChild = NULL;
//   // 
//   // elog(NOTICE, "CacheCustomNext ---------- 1");
//   outerslot = ExecProcNode((PlanState*)linitial( node->custom_ps));

//   elog(NOTICE, "CacheCustomNext ---------- es_processed : %lu ", CacheNode->css.ss.ps.state->es_processed);
//   // elog(NOTICE, "CacheCustomNext ---------- 2");
//   if (TupIsNull(outerslot) ) // retrieve the child n
//   {
//     // elog(NOTICE, "CacheCustomNext ---------- 3");
//     if( CacheNode->childResultinCache && CacheNode->custom_tuple_returned != NULL)
//     {
//       elog(NOTICE, "CacheCustomNext ---------- 4, SAVE SAVE SAVE");
//       printf("Cache: custom_tuple_returned = %lu - is NULL ? %s\n" , CacheNode->custom_tuple_returned->tts_values[0],
//                                                                     CacheNode->custom_tuple_returned->tts_isnull[0]? "true":"false");
//       return CacheNode->custom_tuple_returned;
//     } 
//     // else
//     //   return NULL;
//   }
//   else
//   {
//     // elog(NOTICE, "CacheCustomNext ---------- 4");
//     // if(CacheNode->childResultinCache && CacheNode->NeedModification )
//     // in all cases, all the returned tuple should be entered in the hash and set that it qualifys
//     //{
//       /* TODO : build an entry from the slot returned by the subchild and set that it
//                 qualifies , this entry should be updated in the hash table not added 
//                 I mean it's already exists in the table 
//       */

      
//       // if(CacheNode->css.ss.ps.state->es_processed == 532)
//       // {
//       //   elog(NOTICE, "CacheCustomNext ---------- DEBUG DEBUG");
//       // }
//       // elog(NOTICE, "CacheCustomNext ---------- 5");
//       // ExprContext *tmpcontext;
//       // CacheHashEntry entry = NULL;
//       // TupleTableSlot *firstSlot;

//       // tmpcontext = CacheNode->tmpcontext;
//       // firstSlot = CacheNode->css.ss.ss_ScanTupleSlot;

//       // tmpcontext->ecxt_outertuple = outerslot;
//       // /* Find or build hashtable entry for this outerSlot and modify it to be qualify */
//       // bool isnew = false;
//       // entry = lookup_hash_entry(CacheNode, outerslot, &isnew);
//       // elog(NOTICE, "CacheCustomNext ---------- 6");

//       // if(isnew && entry != NULL)
//       // {
//       //   entry->DoesQualify = true;
//       // }
//       // ExecStoreMinimalTuple(entry->shared.firstTuple,
//       //           firstSlot,
//       //           false);
//       // // I can now return firstSlot
//       // ResetExprContext(tmpcontext);
//       //----------------------
//       // alternative 
//       if(CacheNode->entryFromChild != NULL)
//       {
//         elog(NOTICE, "CacheCustomNext ---------- 6, Entry from Child");
//         printf("Cache: Entry from child %lu", outerslot->tts_values[0]);
//         CacheNode->entryFromChild->DoesQualify = true;
//       }
//       else
//       {
//         elog(NOTICE, "CacheCustomNext ---------- 7, Entry NOT from Child");
//         ExprContext *tmpcontext;
//         CacheHashEntry entry = NULL;
//         TupleTableSlot *firstSlot;

//         tmpcontext = CacheNode->tmpcontext;
//         firstSlot = CacheNode->css.ss.ss_ScanTupleSlot;

//         tmpcontext->ecxt_outertuple = outerslot;
//         /* Find or build hashtable entry for this outerSlot and modify it to be qualify */
//         bool isnew = false;
//         entry = lookup_hash_entry(CacheNode, outerslot, &isnew);
//         // elog(NOTICE, "CacheCustomNext ---------- 6");

//         if(isnew && entry != NULL)
//         {
//           elog(NOTICE, "CacheCustomNext ---------- 8, Entry is new");
//           printf("Cache: Entry is new %lu", outerslot->tts_values[0]);
//           entry->DoesQualify = true;
//         }
//         // ExecStoreMinimalTuple(entry->shared.firstTuple,
//         //           firstSlot,
//         //           false);
//         // I can now return firstSlot
//         ResetExprContext(tmpcontext);
//       }
//       //----------------------
      



//     //}
//   }

//   /* Reset some flags */
//   CacheNode->childResultinCache = false;
//   CacheNode->NeedModification = false;
//   CacheNode->custom_tuple_returned = NULL;
//   CacheNode->entryFromChild = NULL;

//   elog(NOTICE, "CacheCustomNext ---------- finish");
//   return outerslot;
// }

static TupleTableSlot * CacheGenericCustomNext(CustomScanState *node)
{
  TupleTableSlot *outerslot = NULL;
  CacheGenericState * CacheNode = (CacheGenericState * ) node;

  // elog(NOTICE, "CacheCustomNext ---------- start");

  /* some initializing before calling the child */
  // CacheNode->childResultinCache = false;
  // CacheNode->NeedModification = false;
  // CacheNode->custom_tuple_returned = NULL;
  // CacheNode->entryFromChild = NULL;
   
  // elog(NOTICE, "CacheCustomNext ---------- 1");
  outerslot = ExecProcNode((PlanState*)linitial( node->custom_ps));

  
  // elog(NOTICE, "CacheCustomNext ---------- es_processed : %lu ", CacheNode->css.ss.ps.state->es_processed);
  // elog(NOTICE, "CacheCustomNext ---------- 2");
  if (TupIsNull(outerslot) ) // retrieve the child n
  {
    // elog(NOTICE, "CacheCustomNext ---------- 3");
    
    // update the hash
    if(list_length(CacheNode->udefinedtuplesKeys) > 0)
    {
      ListCell * l;
      foreach(l, CacheNode->udefinedtuplesKeys)
      {
        // CacheHashEntry entry = lfirst(l);
        TupleTableSlot * enTuple = (TupleTableSlot *) lfirst(l);

        CacheHashEntry entry = NULL;
        bool isnew = false;
        entry = lookup_hash_entry(CacheNode, enTuple, &isnew);
        // printf("CacheCustomNext ---------- 1, BEFORE: %s\n", entry->undefinedstatus? "undefined" : "defined");
        entry->undefinedstatus = false;
        entry->DoesQualify = false;
        entry->tuple = NULL;
        // printf("CacheCustomNext ---------- 1, undefined entry = %p\n", entry);
        // printf("CacheCustomNext ---------- 1, AFTER: %s\n", entry->undefinedstatus? "undefined" : "defined");
      }
    }
    // list_free_deep(CacheNode->udefinedtuplesKeys);
    list_free(CacheNode->udefinedtuplesKeys);
    CacheNode->udefinedtuplesKeys = NIL;
    ResetExprContext(CacheNode->tmpcontext);

    //DEBUG
    // ListCell * ll;
    // foreach(ll, CacheNode->returnedTuples)
    // {
    //   TupleTableSlot * tt = (TupleTableSlot *) lfirst(ll);
    //   {
    //     // CacheHashEntry entry = (CacheHashEntry) lfirst(ll);
    //      // tt = entry->tuple; 
    //   }
    //   if(tt != NULL)
    //     printf("CacheCustomNext ---------- 2.1, tt = %lu\n", tt->tts_values[0]);
    // }
    //DEBUG

    // return the next tuple
    if(list_length(CacheNode->returnedTuples) > 0)
    {
      // TupleTableSlot * retunredtuple;
      TupleTableSlot * retunredtuple = (TupleTableSlot *)linitial(CacheNode->returnedTuples);
      // CacheHashEntry retunredEntry = (CacheHashEntry )linitial(CacheNode->returnedTuples);
      CacheNode->returnedTuples = list_delete_first(CacheNode->returnedTuples);

      // if(IsA(retunredEntry, CacheHashEntry))
      // {
        // retunredtuple = retunredEntry->tuple;
      // }

      //DEBUG
      // elog(NOTICE, "CacheCustomNext ---------- 3.1, returnedTuple = %lu", retunredtuple->tts_values[0]);
      // printf("\nCacheCustomNext ---------- 3.1, returnedTuple = %lu\n", retunredtuple->tts_values[0]);
      // printf("\nCacheCustomNext ---------- 3.2, mintup = %s\n", retunredtuple->tts_mintuple? "NOT NULL" : "NULL");
      // if(retunredtuple->tts_values[0] == 451000)
      //   elog(NOTICE, "CacheCustomNext ---------- DEBUG DEBUG DEBUG");
      //DEBUG
      return retunredtuple;
    }
  }
  else
  {
    //DEBUG
    // if(outerslot->tts_values[0] == 451000)
    //   elog(NOTICE, "CacheCustomNext ---------- DEBUG DEBUG DEBUG");
    //DEBUG

    //DEBUG
    // ListCell * ll;
    // foreach(ll, CacheNode->returnedTuples)
    // {
    //   TupleTableSlot * tt = (TupleTableSlot *) lfirst(ll);
    //   // {
    //     // CacheHashEntry entry = (CacheHashEntry) lfirst(ll);
    //      // tt = entry->tuple; 
    //   // }
    //   if(tt != NULL)
    //     printf("CacheCustomNext ---------- 8.1, BEFORE 1 tt = %lu\n", tt->tts_values[0]);
    // }
    //DEBUG

    // elog(NOTICE, "CacheCustomNext ---------- 4");
    // update the hash
    if(list_length(CacheNode->udefinedtuplesKeys) > 0)
    {
      ListCell * l;
      foreach(l, CacheNode->udefinedtuplesKeys)
      {
        // CacheHashEntry entry = lfirst(l);
        TupleTableSlot * enTuple = (TupleTableSlot *) lfirst(l);

        CacheHashEntry entry = NULL;
        bool isnew = false;
        entry = lookup_hash_entry(CacheNode, enTuple, &isnew);
        
        entry->undefinedstatus = false;
        entry->DoesQualify = false;
        entry->tuple = NULL;
        // printf("CacheCustomNext ---------- 1, undefined entry = %p\n", entry);
      }
    }

    //DEBUG
    // ListCell * ll;
    // foreach(ll, CacheNode->returnedTuples)
    // {
    //   TupleTableSlot * tt = (TupleTableSlot *) lfirst(ll);
    //   {
    //     // CacheHashEntry entry = (CacheHashEntry) lfirst(ll);
    //      // tt = entry->tuple; 
    //   }
    //   if(tt != NULL)
    //     printf("CacheCustomNext ---------- 8.1, BEFORE 2 tt = %lu\n", tt->tts_values[0]);
    // }
    //DEBUG

    // list_free_deep(CacheNode->udefinedtuplesKeys);
    list_free(CacheNode->udefinedtuplesKeys);
    // elog(NOTICE, "CacheCustomNext ---------- 4.1");
    CacheNode->udefinedtuplesKeys = NIL;

    CacheHashEntry entry = NULL;
    bool isnew = false;
    entry = lookup_hash_entry(CacheNode, outerslot, &isnew);


    // elog(NOTICE, "CacheCustomNext ---------- 8, Entry is new, outerslot %p", outerslot);
    // printf("Cache: Entry is new %lu\n", outerslot->tts_values[0]);

    entry->DoesQualify = true;
    entry->undefinedstatus = false;

    MemoryContext oldCtx;
    oldCtx = MemoryContextSwitchTo(CacheNode->tupContexts);
    TupleTableSlot * tmp =  MakeSingleTupleTableSlot(outerslot->tts_tupleDescriptor);
    tmp->tts_mcxt = CacheNode->tupContexts;
    tmp = ExecCopySlot(tmp , outerslot );

    entry->tuple = tmp;

    

    // add the outerslot to the returned tuples (to be in order )
    CacheNode->returnedTuples = lappend(CacheNode->returnedTuples , tmp);
    
    //DEBUG
    // ListCell * ll;
    // foreach(ll, CacheNode->returnedTuples)
    // {
    //   TupleTableSlot * tt = (TupleTableSlot *) lfirst(ll);
    //   // if(IsA( lfirst(ll), CacheHashEntry))
    //   {
    //     // CacheHashEntry entry = (CacheHashEntry) lfirst(ll);
    //      // tt = entry->tuple; 
    //   }
    //   if(tt != NULL)
    //     printf("CacheCustomNext ---------- 8.1, AFTER tt = %lu\n", tt->tts_values[0]);
    // }
    //DEBUG

    // return the next tuple
    if(list_length(CacheNode->returnedTuples) > 0)
    {
      // elog(NOTICE, "CacheCustomNext ---------- 9");
      
      // TupleTableSlot * retunredtuple;
      TupleTableSlot * retunredtuple = (TupleTableSlot *)linitial(CacheNode->returnedTuples);
      // CacheHashEntry retunredEntry = (CacheHashEntry)linitial(CacheNode->returnedTuples);

      CacheNode->returnedTuples = list_delete_first(CacheNode->returnedTuples);
      
      // if(IsA(retunredEntry, CacheHashEntry))
      // {
        // retunredtuple = retunredEntry->tuple;
      // }

      // elog(NOTICE, "CacheCustomNext ---------- 10");

      //DEBUG
      // elog(NOTICE, "CacheCustomNext ---------- 10.1, returnedTuple = %lu", retunredtuple->tts_values[0]);
      // printf("\nCacheCustomNext ---------- 10.1, returnedTuple = %lu\n", retunredtuple->tts_values[0]);
      // // printf("\nCacheCustomNext ---------- 10.2, mintup = %s\n", retunredtuple->tts_mintuple? "NOT NULL" : "NULL");
      // if(retunredtuple->tts_values[0] == 451000)
      //   elog(NOTICE, "CacheCustomNext ---------- DEBUG DEBUG DEBUG");
      //DEBUG
      return retunredtuple;
    }   
  }

  /* Reset some flags */
  // CacheNode->childResultinCache = false;
  // CacheNode->NeedModification = false;
  // CacheNode->custom_tuple_returned = NULL;
  // CacheNode->entryFromChild = NULL;

  // elog(NOTICE, "CacheCustomNext ---------- finish");
  return outerslot;
}

static bool CacheGenericCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true; 
}

void End_CacheGenericCustomScan (CustomScanState *node)
{
  // elog(NOTICE, "End_CacheGenericCustomScan -------- start");
  
  /*
  * Free the exprcontext
  */
  ExecFreeExprContext(&node->ss.ps);

  /*
  * clean out the upper tuple table
  */
  ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
  * close down subquery
  */

  //End child plans
  // if(list_length(node->custom_ps) == 1)
    ExecEndNode((PlanState*)linitial(node->custom_ps));
  // else //TODO
  // elog(NOTICE, "End_CacheGenericCustomScan -------- finish");
}

void ReScan_CacheGenericCustomScan (CustomScanState *node)
{

  // elog(NOTICE, "ReScan_CachGenericCustomScan -------- start");
  ExecScanReScan(&node->ss);

  /*
  * ExecReScan doesn't know about my subplan, so I have to do
  * changed-parameter signaling myself.  This is just as well, because the
  * subplan has its own memory context in which its chgParam state lives.
  */
  PlanState * childplanstate =  (PlanState *)linitial( node->custom_ps);
  if (node->ss.ps.chgParam != NULL)
    UpdateChangedParamSet(childplanstate, node->ss.ps.chgParam);

  /*
  * if chgParam of subnode is not null then plan will be re-scanned by
  * first ExecProcNode.
  */
  if (childplanstate->chgParam == NULL)
    ExecReScan(childplanstate);

  // elog(NOTICE, "ReScan_CachGenericCustomScan -------- Finish");
}


static void Explain_CacheGenericCustomScan(CustomScanState *node, List *ancestors, ExplainState *es)
{

}


/*
 * Initialize the hash table to empty.
 *
 * The hash table always lives in the aggcontext memory context.
 */
static void
build_hash_table(CacheGenericState *cachestate)
{
  // Agg      *node = (Agg *) aggstate->ss.ps.plan; // ??????? 
  MemoryContext tmpmem = cachestate->tmpcontext->ecxt_per_tuple_memory;
  Size    entrysize;

  // Assert(node->aggstrategy == AGG_HASHED);
  // Assert(node->numGroups > 0);

  entrysize = sizeof(CacheHashEntryData);//sizeof(TupleHashEntryData);  //offsetof(AggHashEntryData, pergroup) +
    // aggstate->numaggs * sizeof(AggStatePerGroupData);

  cachestate->hashtable = BuildTupleHashTable(cachestate->numCols,
                        cachestate->grpColIdx,
                        cachestate->eqfunctions,
                        cachestate->hashfunctions,
                        cachestate->HashTableSize,
                        entrysize,
               cachestate->econtexts->ecxt_per_tuple_memory,
                        tmpmem);
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static CacheHashEntry
lookup_hash_entry(CacheGenericState *aggstate, TupleTableSlot *inputslot, bool * isnew)
{
  TupleTableSlot *hashslot = aggstate->hashslot;
  ListCell   *l;
  CacheHashEntry entry = NULL;
  // bool    isnew = false;

  // elog(NOTICE, "lookup_hash_entry ---------- start");
  /* if first time through, initialize hashslot by cloning input slot */
  if (hashslot->tts_tupleDescriptor == NULL)
  {
    // elog(NOTICE, "lookup_hash_entry ---------- 1");
    
    ExecSetSlotDescriptor(hashslot, inputslot->tts_tupleDescriptor);
    /* Make sure all unused columns are NULLs */
    // elog(NOTICE, "lookup_hash_entry ---------- 2");
    
    ExecStoreAllNullTuple(hashslot);
    
    // elog(NOTICE, "lookup_hash_entry ---------- 3");
  }

  // elog(NOTICE, "lookup_hash_entry ---------- 4");
  /* transfer just the needed columns into hashslot */
  slot_getsomeattrs(inputslot, linitial_int(aggstate->hash_needed));

  // elog(NOTICE, "lookup_hash_entry ---------- 5");
  
  foreach(l, aggstate->hash_needed)
  {
    int     varNumber = lfirst_int(l) - 1;

    // elog(NOTICE, "lookup_hash_entry ---------- 6");
    hashslot->tts_values[varNumber] = inputslot->tts_values[varNumber];
    hashslot->tts_isnull[varNumber] = inputslot->tts_isnull[varNumber];

    // elog(NOTICE, "lookup_hash_entry ---------- tts_values = %d , tts_isnull= %s" , 
    //       DatumGetInt16(hashslot->tts_values[0]) , 
    //        (hashslot->tts_isnull[varNumber])? "true" : "false"      );
    // printf("value = %d \n" , DatumGetInt16(hashslot->tts_values[0]));

    // elog(NOTICE, "lookup_hash_entry ---------- 7");
  }

  // elog(NOTICE, "lookup_hash_entry ---------- 8");
  
  //DEBUG
  // bool *isnew2 = NULL;
  //DEBUG

  /* find or create the hashtable entry using the filtered tuple */
  entry = (CacheHashEntry) LookupTupleHashEntry(aggstate->hashtable,
                        hashslot,
                        isnew);

  // elog(NOTICE, "lookup_hash_entry ---------- 9 , entry = %s" , (entry)? "NOT NULL" : "NULL");
  // elog(NOTICE, "lookup_hash_entry ---------- 9");

  // if (*isnew && entry != NULL)
  // {
    // elog(NOTICE, "lookup_hash_entry ---------- 10");
    /* initialize aggregates for new tuple group */
    // initialize_aggregates(aggstate, entry->pergroup, 0);
    // entry->DoesQualify = true;
  // }


  // elog(NOTICE, "lookup_hash_entry ---------- FINISH");
  return entry;
}
//============================================================
//============================================================

CustomPath* create_materialCustom_path(Path* currentPath , Path* childPath)
{
  elog(NOTICE, "create_materialCustom_path ----------- start , relid= %d " , currentPath->parent->relid);
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  
  RelOptInfo * rel = currentPath->parent;
  pathnode->path.parent = rel;
  // pathnode->path.pathtarget = currentPath->parent->reltarget; // currentPath->pathtarget ??

  pathnode->custom_paths = lappend(pathnode->custom_paths, childPath);    
  pathnode->path.param_info = currentPath->param_info; 

  pathnode->path.pathtarget = currentPath->pathtarget;
  pathnode->path.parallel_aware = false;
  pathnode->path.parallel_safe = currentPath->parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
  pathnode->path.parallel_workers = currentPath->parallel_workers;
  pathnode->path.pathkeys = currentPath->pathkeys; // ?? or should I put the child pathkeys
  
  /* compute the cost */
  pathnode->path.rows = currentPath->rows;     /* estimated number of result tuples */
  pathnode->path.startup_cost = currentPath->startup_cost; /* cost expended before fetching any tuples */
  pathnode->path.total_cost = currentPath->total_cost;   /* TODO: need to estimate the cost ?? */

  /* set the value of K (Limit Value) */
  SubqueryScanPath * currSubqueryPath = (SubqueryScanPath *) currentPath;
  Assert(IsA(currSubqueryPath, SubqueryScanPath));
  Assert(IsA(currSubqueryPath->subpath, LimitPath));
  
  /* TODO: for current, there is only one limit in the query so definitly, 
            root->limit_count , and limitOffset are representing the K */
  pathnode->custom_private =NIL; 
  
  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "MaterialCustomScan";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_MaterialCustomPath;
  pathnode->methods = methods;
  
  

  //DEBUG
  printf("\ncreate_materialCustom_path ============ childPath: \n");
  pprint(childPath);

  printf("\ncreate_materialCustom_path ============ currentPath: \n");
  pprint(currentPath);

  printf("create_materialCustom_path ============= CustomPath:\n");
  pprint(pathnode);
  //DEBUG

  return pathnode;
}



// moodify the planner for post-Filter KNN-Select
void my_set_relpathlist3(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{
  Query    *parse = root->parse;
  // elog(NOTICE, "my_set_relpathlist3 ------------ start");
  /*
   * if rel has spgist index col , 
   * if root->sortkey is <-> operator with Point type 
   *.   then add index scan path to the rel
  */
  bool isorderby = false;
  bool HasRelSPGIndex = false;
  bool isCurRel = false;
  AttrNumber  operatorAttrno = 0 ;
  OpExpr * orderybyCaluse = NULL;

  /* check if there is Order By operator is used with Point data type*/
  ListCell *lc;
  foreach(lc , root->sort_pathkeys)
  {
    PathKey * key = lfirst(lc);
    if(key != NULL && key->pk_eclass !=NULL)
    {
      EquivalenceClass * eclass = (EquivalenceClass *)key->pk_eclass;
      ListCell *l;
      foreach(l , eclass->ec_members)
      {
        EquivalenceMember * emember = (EquivalenceMember * ) lfirst(l);
        OpExpr * opr = (OpExpr *) emember->em_expr;

        if(IsA(opr , OpExpr) && opr->opno == 517  && list_length(opr->args) == 2 ) 
        {
          Var      *leftvar;
          Const    *rightvar;
          leftvar = (Var *) get_leftop((Expr *) opr);
          rightvar = (Const *) get_rightop((Expr *) opr);

          if( IsA(rightvar , Const) && rightvar->consttype == 600 && !rightvar->constisnull)
          {
             // Point queryPoint = DatumGetPointP(rightvar->constvalue);
             isorderby = true;
             orderybyCaluse = opr;
            // elog(NOTICE, "my_set_relpathlist3 ------------ 1");
             /* check if this operator is used on the same current rel ? */
            if(bms_equal(rel->relids ,emember->em_relids  ))
              isCurRel = true;
          }
          if(IsA(leftvar , Var))
          {
            // elog(NOTICE, "my_set_relpathlist3 ------------ 1 :  leftVar= %d", leftvar->varattno);
            operatorAttrno = leftvar->varattno;
          }
        } 
      }
    }
  }
  // elog(NOTICE, "my_set_relpathlist3 ------------ 2");
  /* check if relation has SpGist quad column with ordery by operator */
  if(isorderby && isCurRel)
  {
    // elog(NOTICE, "my_set_relpathlist3 ------------ 2.1");
    lc = NULL;
    foreach(lc , rel->indexlist)
    {
      // elog(NOTICE, "my_set_relpathlist3 ------------ 2.2");
      IndexOptInfo * indexInfo = (IndexOptInfo *) lfirst(lc);
      
      // printf("my_set_relpathlist3 ------------indexInfo:\n");
      // pprint(indexInfo);
      if(!indexInfo->amcanorderbyop)
        continue;

      ListCell *l;
      foreach(l , indexInfo->indextlist)
      {
        // elog(NOTICE, "my_set_relpathlist3 ------------ 2.3");
        TargetEntry * entry = (TargetEntry *) lfirst(l);
        Var * ex = (Var *) entry->expr;
        if(IsA(ex , Var) )
        {
          // elog(NOTICE, "my_set_relpathlist3 ------------ 1 :  entryNo= %d", entry->resno);
          
          if(ex->varattno ==  operatorAttrno )
          {
            HasRelSPGIndex = true;

            //====================================
            /* decide wthether to add QEP2 for postfilter or not */
            // double selectivity = 0.0;
            // int k = (root->limit_tuples > 0)? root->limit_tuples : root->tuple_fraction; 
            // if(parse->jointree)
            // {
            //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.01");
            //   ListCell * cell1;
            //   Node* ptr= NULL;
            //   int id = 0;
            //   foreach(cell1 , (List*)parse->jointree->quals)
            //   {
            //     OpExpr * op = (OpExpr*) lfirst(cell1);
            //     if( IsA( op , OpExpr) || IsA( op , Expr))
            //     {
                  
            //       elog(NOTICE, "my_set_relpathlist3 ------------ 1.04");
            //       if(list_length(op->args) == 2 && IsA(get_leftop((Expr *)op) , Var)
            //                                       && IsA(get_rightop((Expr *)op), Const) )
            //       {
            //         elog(NOTICE, "my_set_relpathlist3 ------------ 1.01 : call scalargtsel fn. ");  
            //         selectivity = DatumGetFloat8(DirectFunctionCall4(scalargtsel, 
            //                     PointerGetDatum(root) , 
            //                     ObjectIdGetDatum(op->opno),
            //                     PointerGetDatum(op->args),
            //                     0)); //Int32GetDatum(relid) ) );

            //         elog(NOTICE, "my_set_relpathlist3 ------------ 1.01 : selectivity= %f", selectivity);  
            //       }
            //     }
            //   }

            //   // for(; id < list_length((List*)parse->jointree->quals) ; id++)
            //   // {
            //   //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.02");
            //   //   ptr = (parse->jointree->quals);
            //   //   if(ptr == NULL)
            //   //     break;

            //   //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.03");

            //   //   OpExpr * op = (OpExpr*) ptr;
            //   //   pprint(op);
            //   //   if( IsA( op , OpExpr) || IsA( op , Expr))
            //   //   {
                  
            //   //     elog(NOTICE, "my_set_relpathlist3 ------------ 1.04");
            //   //     if(list_length(op->args) == 2 && IsA(get_leftop((Expr *)op) , Var)
            //   //                                     && IsA(get_rightop((Expr *)op), Const) )
            //   //     {
            //   //       elog(NOTICE, "my_set_relpathlist3 ------------ 1.01 : call scalargtsel fn. ");  
            //   //       double selectivity = DatumGetFloat8(DirectFunctionCall4(scalargtsel, 
            //   //                   PointerGetDatum(root) , 
            //   //                   ObjectIdGetDatum(op->opno),
            //   //                   PointerGetDatum(op->args),
            //   //                   0)); //Int32GetDatum(relid) ) );

            //   //       elog(NOTICE, "my_set_relpathlist3 ------------ 1.01 : selectivity= %f", selectivity);  
            //   //     }
            //   //   }

            //   //   ptr++;
            //   // }

              
            // }
            // if(selectivity < 0.20) // 20%
            // {
            //   return; // will not use the Postfilter optimization
            // }
            // else 
            // if(selectivity != 1)
            // {
            //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.06");
            //   double diff = 1 - selectivity;
            //   k = k / diff ;
            //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.06 : new K= %d", k);
            // }
            //====================================

            /* step 1: preprocess root and rel , so the ordered index scan is considered */

              RestrictInfo * rinfo = make_restrictinfo((Expr*)orderybyCaluse,
                                                    false,false,false,rel->relids,NULL,NULL);
              

              //DEBUG
              // elog(NOTICE , "\n\n Collation = %d  - collation = %d\n\n", orderybyCaluse->inputcollid, ((OpExpr *)rinfo->clause)->inputcollid);
              // printf("\n------------------------------------my_set_relpathlist3:rinfo: \n");
              // pprint(rinfo); 
              //DEBUG
      
              
              /* add order by pathkey to root->query_pathkey */
              ListCell *ll ;
              PathKey * chosen = NULL;
              foreach(ll, root->canon_pathkeys)
              {
                PathKey *pkey = lfirst(ll);
                // TODO: for current, I'll check the pk_opfamily only, but I need to make sure it's a <-> operator with point operarnd
                if(pkey->pk_opfamily == 1970)
                  chosen = pkey;
              }
              
              if(chosen)
              {
                
                if(!list_member_ptr(root->query_pathkeys , chosen))
                {
                  // elog(NOTICE, "my_set_relpathlist3 ------------ 1.1 : pathkey is not a member of root->query_pathkeys List");
                  
                  while(root->query_pathkeys != NIL)
                    root->query_pathkeys = list_delete_first(root->query_pathkeys);

                  // root->query_pathkeys = lappend(root->query_pathkeys , chosen);
                  root->query_pathkeys = lcons( chosen, root->query_pathkeys );
                }
                // if(!list_member_ptr(root->group_pathkeys , chosen))
                // {
                //   // elog(NOTICE, "my_set_relpathlist3 ------------ 1.2 : pathkey is not a member of root->group_pathkeys List");
                //   // root->group_pathkeys = lcons(  chosen, root->group_pathkeys);
                // }
                // else
                // {
                //   // change the order (order by should be first )
                //   elog(NOTICE, "my_set_relpathlist3 ------------ 1.3 : change the order (order by should be first in group_pathkeys)");
                //   // root->group_pathkeys = list_delete(root->group_pathkeys , chosen);
                //   // root->group_pathkeys = lcons(  chosen, root->group_pathkeys); 
                // }
              }
              
              // printf("my_set_relpathlist3 ------------ Root Before create_index_paths\n");
              // pprint(root);
            

             //===================================================
             //===================================================
             List *indexorderbys = NIL;
             List *indexorderbycols = NIL;
             List *pathkeys = NIL;
             bool index_is_ordered;

             indexorderbys = lappend(indexorderbys, rinfo->clause);
             indexorderbycols = lappend_int(indexorderbycols, 0);
             pathkeys = lappend(pathkeys , chosen);
             index_is_ordered = (indexInfo->sortopfamily != NULL);

             // add reltarget to have order by col
             RelOptInfo * indexrel = indexInfo->rel;
             
             IndexPath * ipath = create_index_path(root, 
                                                    indexInfo, 
                                                    NIL,
                                                    NIL,
                                                    indexorderbys,
                                                    indexorderbycols,
                                                    pathkeys,
                                                    index_is_ordered ? 
                                                    ForwardScanDirection :
                                                    NoMovementScanDirection,
                                                    false, // indexOnly
                                                    NULL,  // required_outer
                                                    1.0 );
             add_path(rel, (Path*)ipath);
            //===================================================
            //===================================================
            
            //DEBUG 
            // elog(NOTICE, "my_set_relpathlist3 ------------ 5");
            // printf("my_set_relpathlist3 ------------ Rel After create_index_paths: \n");
            // pprint(rel);
            // printf("my_set_relpathlist3 ------------ Root After create_index_paths: \n");
            // pprint(root);
            
            //dEBUG

            return;
          }
        }
      }
    }
  }
  // elog(NOTICE, "my_set_relpathlist3 ------------ END");
  return;
  /* 
   * TODO: add the order by operator to the join clause so the index scan should 
   * be added to the rel pathlist and considered in Joining 
  */

  /* Add the RestrictInfo to rel->joinInfo  that represent Order By index scan */
  // TODO
  // if(HasRelSPGIndex && isorderby && isCurRel )
  // {

 
  // }
}




void my_set_relpathlist2(PlannerInfo *root,
                            RelOptInfo *rel,
                            Index rti,
                            RangeTblEntry *rte)
{
  Query    *parse = root->parse;
  Query    *subquery = rte->subquery;
   // elog(NOTICE , "my_set_relpathlist2 =================== start");
  // printf("\n------------------------------------Root:\n");
  // pprint(root);
  // printf("\n------------------------------------Root->processed_tlist:\n");
  // pprint(root->processed_tlist);
  // printf("\n------------------------------------Parse:\n");
  // pprint(parse);
  // printf("\n------------------------------------Rel:\n");
  // pprint(rel);
  
  if(subquery)   //this means we are in the top query not the subquery 
  {

    // printf(" \n\nWe are in Top Query .... Return\n\n");
    return;
    
  }
  else
  {
     // elog(NOTICE,"my_set_relpathlist2 =================== 1");
    PlannerInfo *parent_root = root->parent_root;

    if(!parent_root) return;

    RangeTblEntry *_rte;// = parent_root->simple_rte_array[1];
    // subquery = _rte->subquery;
    
    // printf("\n------------------------------------Parent_root:\n");
    // pprint(parent_root);
    
    int iter;
    for(iter = 1; iter < parent_root->simple_rel_array_size; iter ++)
    {
      // printf("\n------------------------------------:simple_rte_array[%d]\n", iter);
      // pprint(parent_root->simple_rte_array[iter]);
      if(parent_root->simple_rte_array[iter]->subquery) 
      {
        _rte = parent_root->simple_rte_array[iter];
        subquery = _rte->subquery;
      }
    }
    // elog(NOTICE, "my_set_relpathlist2 =================== 1.1");
    if(!subquery) return;
    
     // elog(NOTICE, "my_set_relpathlist2 =================== 2");
    // RelOptInfo *parent_rel = parent_root->simple_rel_array[1];


    //push down the predicates 
    // if (parent_rel->baserestrictinfo != NIL )
    // {
    //   /* OK to consider pushing down individual quals */
    //   List     *upperrestrictlist = NIL;
    //   ListCell   *l;
    //   foreach(l, parent_rel->baserestrictinfo)
    //   {
    //     RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
    //     Node     *clause = (Node *) rinfo->clause;
    //     if (!rinfo->pseudoconstant )
    //     {/* Push it down */
    //       if(IsA(clause , OpExpr))
    //       {
    //           OpExpr * qual = (OpExpr *)clause;
              
    //           ListCell * lc2;
    //           foreach(lc2 , qual->args)
    //           {
    //             if(IsA(lfirst(lc2) , Var) )
    //             {
    //               Var * v = lfirst(lc2);
    //               AttrNumber varattno_old = v->varattno;

    //               int i = 1;
    //               ListCell *lc3;
    //               foreach(lc3 , rel->reltarget->exprs)
    //               {
    //                 if(i == varattno_old)
    //                 {
    //                   elog(NOTICE , "\n\n ^_^ ^_^  set varattno : \n\n");
    //                   Var * v_dest = lfirst(lc3);
    //                   v->varattno =  v_dest->varattno;
    //                 }
    //                 i++;
    //               }
    //             }
    //           }
              
    //           rinfo = make_restrictinfo((Expr *)clause , true ,false , false, parent_rel->relids,rel->relids,NULL);
    //           rel->baserestrictinfo = lappend(rel->baserestrictinfo , rinfo);
    //         }
    //     }
    //     else
    //     {/* Keep it in the upper query */
    //       upperrestrictlist = lappend(upperrestrictlist, rinfo);
    //     }
    //   }
    //   parent_rel->baserestrictinfo = upperrestrictlist;
    // }

    // remove the 4th output in reltarget (processed_tlist) that correspond to the operator <->
    ListCell *lc;
    ListCell *next = NULL;
    ListCell *prev = NULL;

    OpExpr * KNN_op = NULL;
    for(lc = list_head(root->processed_tlist) ; lc ; lc = next)
    {
      next = lnext(lc);
      TargetEntry *node = lfirst(lc);
      if(IsA(node->expr , OpExpr))
      {
        
        
        if( ((OpExpr*)(node->expr))->opno == 517) // <-> operator
        {
          KNN_op = copyObject(node->expr);
          // elog(NOTICE , "my_set_relpathlist2=================== node is OpExpr 517 : delete it");
          root->processed_tlist = list_delete_cell(root->processed_tlist , lc , prev);
        }

         
      }
      else
        prev = lc;
    }
    // elog(NOTICE, "my_set_relpathlist2 =================== 3");
    // remove sort clause operator
    if(parse->sortClause)
      parse->sortClause = NIL;
    
    //Don't remove it 
    //remove Limit count 
    int k = 0;
    if(parse->limitCount && IsA(parse->limitCount , Const))
    {
      // ((Const *)parse->limitCount)->constisnull = true; 
      // int length = ((Const *)parse->limitCount)->constlen;
      //if(length == 8)
        k = DatumGetUInt8(((Const *)parse->limitCount)->constvalue);
      
    }

    set_baserel_size_estimates(root , rel);

    // elog(NOTICE, "my_set_relpathlist2 =================== 4");
    //TODO: remove other paths: 
    //remove the other paths (for NOW only)
    prev = NULL;
    next = NULL;
    for( lc = list_head(rel->pathlist) ; lc != NULL ; lc = next)
    {
      next = lnext(lc);
      Path * p = lfirst(lc);
      switch (p->pathtype)
      {
        case T_IndexScan:
          // elog(NOTICE, "my_set_relpathlist2 =================== 5");
          rel->pathlist = list_delete_cell(rel->pathlist , lc , prev);
        break;
        case T_SeqScan:
          // elog(NOTICE, "my_set_relpathlist2 =================== 6");
          rel->pathlist = list_delete_cell(rel->pathlist , lc , prev);
        break; 
        default:
          // elog(NOTICE, "my_set_relpathlist2 =================== 7");
          prev = lc;
        break;   
      }
    }

    Relids required_outer;
    required_outer = rel->lateral_relids;

    // elog(NOTICE, "my_set_relpathlist2 =================== 8");
    //retreive important needed for our KNN path 

    //add_path(rel , create_seqscan_path(root, rel, required_outer, 0));
    add_path(rel , (Path *) create_basicCustomScan_path(root, rel, NULL, KNN_op, k));
    // elog(NOTICE, "my_set_relpathlist2 =================== 9");
  }
    // printf("\nmy_set_relpathlist2=================== rel");
    // pprint(rel);
  // elog(NOTICE, "my_set_relpathlist2 =================== end");
}




/* Version 1 */
// KInCircleJoinPath* create_kInCircle_Join_path(PlannerInfo *root, JoinPath * currentPath)
// {
//   elog(NOTICE, "create_kInCircle_Join_path ----------- start , relid= %d " , currentPath->path.parent->relid);

//   KInCircleJoinPath *  KInCircleJoinPathnode = malloc(sizeof(KInCircleJoinPathnode));//makeNode(CustomPath);
//   CustomPath * pathnode = & KInCircleJoinPathnode->custompath;
  
//   pathnode->path.pathtype = T_CustomScan;
  
//   RelOptInfo * rel = currentPath->path.parent;
//   pathnode->path.parent = rel;
  

//   // Assert(IsA(currentPath->innerjoinpath, CustomPath)); // TODO: I should do this check earlier
//   // pathnode->custom_paths = lappend(pathnode->custom_paths, currentPath->outerjoinpath);    
//   // pathnode->custom_paths = lappend(pathnode->custom_paths, currentPath->innerjoinpath);    
//   pathnode->custom_paths = NIL;
//   KInCircleJoinPathnode->outerjoinpath = currentPath->outerjoinpath;
//   KInCircleJoinPathnode->innerjoinpath = currentPath->innerjoinpath;
//   KInCircleJoinPathnode->joinrestrictinfo = currentPath->joinrestrictinfo;
//   KInCircleJoinPathnode->jointype = currentPath->jointype;


//   pathnode->path.param_info = currentPath->path.param_info; 

//   pathnode->path.pathtarget = currentPath->path.pathtarget;
//   pathnode->path.parallel_aware = false;
//   pathnode->path.parallel_safe = currentPath->path.parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
//   pathnode->path.parallel_workers = currentPath->path.parallel_workers;
//   pathnode->path.pathkeys = currentPath->path.pathkeys; // ?? or should I put the child pathkeys
  
//   /* compute the cost */
//   pathnode->path.rows = currentPath->path.rows;     /* estimated number of result tuples */
//   pathnode->path.startup_cost = currentPath->path.startup_cost; /* cost expended before fetching any tuples */
//   pathnode->path.total_cost = currentPath->path.total_cost;   /* TODO: need to estimate the cost ?? */

//   root->curOuterRels = bms_add_member(root->curOuterRels , currentPath->outerjoinpath->parent->relid);

//   pathnode->custom_private =NIL; 
  
//   struct CustomPathMethods * methods;
//   methods = palloc(sizeof(* methods));
//   char * name = palloc(sizeof(20));
//   name = "kInCircleJoinCustomScan";
//   methods->CustomName = name;
//   methods->PlanCustomPath = Plan_KInCircleJoinCustomPath;
//   pathnode->methods = methods;
  
  

//   //DEBUG
//   // printf("\ncreate_kInCircle_Join_path ============ childPath: \n");
//   // pprint(childPath);

//   printf("\ncreate_kInCircle_Join_path ============ currentPath: \n");
//   pprint(currentPath);

//   printf("create_kInCircle_Join_path ============= CustomPath:\n");
//   pprint( (CustomPath *) KInCircleJoinPathnode);
//   //DEBUG

//   return KInCircleJoinPathnode;
// }

/* Version 2 */
// CustomPath* create_kInCircle_Join_path(PlannerInfo *root, JoinPath * currentPath)
// {
//   elog(NOTICE, "create_kInCircle_Join_path ----------- start , relid= %d " , currentPath->path.parent->relid);

//   // KInCircleJoinPath *  KInCircleJoinPathnode = malloc(sizeof(KInCircleJoinPathnode));//makeNode(CustomPath);
//   // CustomPath * pathnode = & KInCircleJoinPathnode->custompath;
//   CustomPath * pathnode =makeNode(CustomPath);
//   pathnode->path.pathtype = T_CustomScan;
  
//   RelOptInfo * rel = currentPath->path.parent;
//   pathnode->path.parent = rel;
  

//   // Assert(IsA(currentPath->innerjoinpath, CustomPath)); // TODO: I should do this check earlier
//   pathnode->custom_paths = lappend(pathnode->custom_paths, currentPath->outerjoinpath);    
//   pathnode->custom_paths = lappend(pathnode->custom_paths, currentPath->innerjoinpath);    
//   // KInCircleJoinPathnode->outerjoinpath = currentPath->outerjoinpath;
//   // KInCircleJoinPathnode->innerjoinpath = currentPath->innerjoinpath;
//   // KInCircleJoinPathnode->joinrestrictinfo = currentPath->joinrestrictinfo;
//   // KInCircleJoinPathnode->jointype = currentPath->jointype;

//   pathnode->path.param_info = currentPath->path.param_info; 

//   // pathnode->path.pathtarget = currentPath->path.pathtarget;
//   pathnode->path.pathtarget = rel->reltarget;
//   pathnode->path.parallel_aware = false;
//   pathnode->path.parallel_safe = currentPath->path.parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
//   pathnode->path.parallel_workers = currentPath->path.parallel_workers;
//   pathnode->path.pathkeys = currentPath->path.pathkeys; // ?? or should I put the child pathkeys
  
//   /* compute the cost */
//   pathnode->path.rows = currentPath->path.rows;     /* estimated number of result tuples */
//   pathnode->path.startup_cost = currentPath->path.startup_cost; /* cost expended before fetching any tuples */
//   pathnode->path.total_cost = currentPath->path.total_cost;   /* TODO: need to estimate the cost ?? */

  
//   pathnode->custom_private =NIL; 
//   // pathnode->custom_private = lappend(pathnode->custom_private , currentPath->joinrestrictinfo); 
  
//   // root->curOuterRels = bms_add_member(root->curOuterRels , currentPath->outerjoinpath->parent->relid);

//   struct CustomPathMethods * methods;
//   methods = palloc(sizeof(* methods));
//   char * name = palloc(sizeof(20));
//   name = "kInCircleJoinCustomScan";
//   methods->CustomName = name;
//   methods->PlanCustomPath = Plan_KInCircleJoinCustomPath;
//   pathnode->methods = methods;
  
  

//   //DEBUG
//   // printf("\ncreate_kInCircle_Join_path ============ childPath: \n");
//   // pprint(childPath);

//   printf("\ncreate_kInCircle_Join_path ============ currentPath: \n");
//   pprint(currentPath);

//   printf("create_kInCircle_Join_path ============= CustomPath:\n");
//   pprint(pathnode);
//   //DEBUG

//   return pathnode;
// }

/* version 3 */
CustomPath* create_kInCircle_Join_path2(PlannerInfo *root, JoinPath * currentPath, Path * innerPath)
{
  elog(NOTICE, "create_kInCircle_Join_path ----------- start , relid= %d " , currentPath->path.parent->relid);

  // KInCircleJoinPath *  KInCircleJoinPathnode = malloc(sizeof(KInCircleJoinPathnode));//makeNode(CustomPath);
  // CustomPath * pathnode = & KInCircleJoinPathnode->custompath;
  CustomPath * pathnode =makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  
  RelOptInfo * rel = currentPath->path.parent;
  pathnode->path.parent = rel;
  

  // Assert(IsA(currentPath->innerjoinpath, CustomPath)); // TODO: I should do this check earlier
  pathnode->custom_paths = lappend(pathnode->custom_paths, currentPath->outerjoinpath);    
  pathnode->custom_paths = lappend(pathnode->custom_paths, innerPath);    
  // KInCircleJoinPathnode->outerjoinpath = currentPath->outerjoinpath;
  // KInCircleJoinPathnode->innerjoinpath = currentPath->innerjoinpath;
  // KInCircleJoinPathnode->joinrestrictinfo = currentPath->joinrestrictinfo;
  // KInCircleJoinPathnode->jointype = currentPath->jointype;

  pathnode->path.param_info = currentPath->path.param_info; 

  // pathnode->path.pathtarget = currentPath->path.pathtarget;
  pathnode->path.pathtarget = rel->reltarget;
  pathnode->path.parallel_aware = false;
  pathnode->path.parallel_safe = currentPath->path.parallel_safe; //rel->consider_parallel && childpath->parallel_safe;
  pathnode->path.parallel_workers = currentPath->path.parallel_workers;
  pathnode->path.pathkeys = currentPath->path.pathkeys; // ?? or should I put the child pathkeys
  
  /* compute the cost */
  pathnode->path.rows = currentPath->path.rows;     /* estimated number of result tuples */
  pathnode->path.startup_cost = currentPath->path.startup_cost; /* cost expended before fetching any tuples */
  pathnode->path.total_cost = currentPath->path.total_cost;   /* TODO: need to estimate the cost ?? */

  
  pathnode->custom_private =NIL; 
  // pathnode->custom_private = lappend(pathnode->custom_private , currentPath->joinrestrictinfo); 
  
  // root->curOuterRels = bms_add_member(root->curOuterRels , currentPath->outerjoinpath->parent->relid);

  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "kInCircleJoinCustomScan";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_KInCircleJoinCustomPath;
  pathnode->methods = methods;
  
  

  //DEBUG
  // printf("\ncreate_kInCircle_Join_path ============ childPath: \n");
  // pprint(childPath);

  printf("\ncreate_kInCircle_Join_path ============ currentPath: \n");
  pprint(currentPath);

  printf("create_kInCircle_Join_path ============= CustomPath:\n");
  pprint(pathnode);
  //DEBUG

  return pathnode;
}


typedef struct {
  CustomScan customscanplan;
  JoinType  jointype;
  List     *joinqual;    /*JOIN quals (in addition to plan.qual) */
  List     *nestParams;   /* list of NestLoopParam nodes */

}KInCircleJoinPlan;

// static KInCircleJoinPlan * make_KInCircleJoinPlan(List *tlist,List *joinclauses,List *otherclauses,List *nestParams,Plan *lefttree,Plan *righttree,JoinType jointype);

// KInCircleJoinPlan * create_KInCircleJoinPlan(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
// {
//   // NestLoop   *join_plan;
//   KInCircleJoinPlan   *join_plan;
//   Plan     *outer_plan;
//   Plan     *inner_plan;
//   // List     *tlist = build_path_tlist(root, &best_path->path);

//   KInCircleJoinPath * KInCircle_best_path = (KInCircleJoinPath *) best_path;
//   List     *joinrestrictclauses = KInCircle_best_path->joinrestrictinfo;
//   List     *joinclauses;
//   List     *otherclauses;
//   Relids    outerrelids;
//   List     *nestParams;
//   Relids    saveOuterRels = root->curOuterRels;
//   ListCell   *cell;
//   ListCell   *prev;
//   ListCell   *next;

//   /* NestLoop can project, so no need to be picky about child tlists */
//   outer_plan = create_plan_recurse(root, KInCircle_best_path->outerjoinpath, 0);

//   /* For a nestloop, include outer relids in curOuterRels for inner side */
//   root->curOuterRels = bms_union(root->curOuterRels,
//                    KInCircle_best_path->outerjoinpath->parent->relids);

//   inner_plan = create_plan_recurse(root, KInCircle_best_path->innerjoinpath, 0);

//   /* Restore curOuterRels */
//   bms_free(root->curOuterRels);
//   root->curOuterRels = saveOuterRels;

//   /* Sort join qual clauses into best execution order */
//   joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

//   /* Get the join qual clauses (in plain expression form) */
//   /* Any pseudoconstant clauses are ignored here */
//   if (IS_OUTER_JOIN(KInCircle_best_path->jointype))
//   {
//     extract_actual_join_clauses(joinrestrictclauses,
//                   &joinclauses, &otherclauses);
//   }
//   else
//   {
//     /* We can treat all clauses alike for an inner join */
//     joinclauses = extract_actual_clauses(joinrestrictclauses, false);
//     otherclauses = NIL;
//   }

//   /* Replace any outer-relation variables with nestloop params */
//   if (best_path->path.param_info)
//   {
//     joinclauses = (List *)
//       replace_nestloop_params(root, (Node *) joinclauses);
//     otherclauses = (List *)
//       replace_nestloop_params(root, (Node *) otherclauses);
//   }

//   /*
//    * Identify any nestloop parameters that should be supplied by this join
//    * node, and move them from root->curOuterParams to the nestParams list.
//    */
//   outerrelids = KInCircle_best_path->outerjoinpath->parent->relids;
//   nestParams = NIL;
//   prev = NULL;
//   for (cell = list_head(root->curOuterParams); cell; cell = next)
//   {
//     NestLoopParam *nlp = (NestLoopParam *) lfirst(cell);

//     next = lnext(cell);
//     if (IsA(nlp->paramval, Var) &&
//       bms_is_member(nlp->paramval->varno, outerrelids))
//     {
//       root->curOuterParams = list_delete_cell(root->curOuterParams,
//                           cell, prev);
//       nestParams = lappend(nestParams, nlp);
//     }
//     else if (IsA(nlp->paramval, PlaceHolderVar) &&
//          bms_overlap(((PlaceHolderVar *) nlp->paramval)->phrels,
//                outerrelids) &&
//          bms_is_subset(find_placeholder_info(root,
//                       (PlaceHolderVar *) nlp->paramval,
//                            false)->ph_eval_at,
//                  outerrelids))
//     {
//       root->curOuterParams = list_delete_cell(root->curOuterParams,
//                           cell, prev);
//       nestParams = lappend(nestParams, nlp);
//     }
//     else
//       prev = cell;
//   }

//   join_plan = make_KInCircleJoinPlan(tlist,
//                 joinclauses,
//                 otherclauses,
//                 nestParams,
//                 outer_plan,
//                 inner_plan,
//                 best_path->jointype);

//   // copy_generic_path_info(&join_plan->join.plan, &best_path->path);
//   join_plan->customscanplan.scan.plan->startup_cost = best_path->path.startup_cost;
//   join_plan->customscanplan.scan.plan->total_cost = best_path->path.total_cost;
//   join_plan->customscanplan.scan.plan->plan_rows = best_path->path.rows;
//   join_plan->customscanplan.scan.plan->plan_width = best_path->path.pathtarget->width;
//   join_plan->customscanplan.scan.plan->parallel_aware = best_path->path.parallel_aware;
//   join_plan->customscanplan.flags = best_path->flags;

//   return join_plan;
// }

// static KInCircleJoinPlan *
// make_KInCircleJoinPlan(List *tlist,
//         List *joinclauses,
//         List *otherclauses,
//         List *nestParams,
//         Plan *lefttree,
//         Plan *righttree,
//         JoinType jointype)
// {
//   // NestLoop   *node = makeNode(NestLoop);
//   KInCircleJoinPlan * node = malloc(sizeof(KInCircleJoinPlanNode));
//   CustomScan * customscanNode = & node->customscanplan; 
//   Plan     *plan = &customscanNode.scan.plan;
  
//   // Plan     *plan = &node->join.plan;

//   plan->targetlist = tlist;
//   plan->qual = otherclauses;
//   plan->lefttree = lefttree;
//   plan->righttree = righttree;
//   node->jointype = jointype;
//   node->joinqual = joinclauses;
//   node->nestParams = nestParams;

//   struct CustomScanMethods * methods;
//   methods = palloc(sizeof(* methods));
//   char * name = palloc(sizeof(20));
//   name = "KInCircleJoinCustomScanPlan";
//   methods->CustomName = name;
//   methods->CreateCustomScanState = create_KInCircleJoinCustomScan_state;
//   customscanNode->methods = methods;


//   return node;
// }

static Plan * Plan_KInCircleJoinCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  elog(NOTICE, "Plan_KInCircleJoinCustomPath ----------- start , relid= %d " , rel->relid);
  //DEBUG
  printf("\nPlan_KInCircleJoinCustomPath ======== rel:\n");
  pprint(rel);
  printf("\nPlan_KInCircleJoinCustomPath ======== best_path:\n");
  pprint(best_path);
  printf("\nPlan_KInCircleJoinCustomPath ======== tlist:\n");
  pprint(tlist);
  printf("\nPlan_KInCircleJoinCustomPath ======== clauses:\n");
  pprint(clauses);
  printf("\nPlan_KInCircleJoinCustomPath ======== custom_plans:\n");
  pprint(custom_plans);
  //DEBUG

  // return (Plan *)create_KInCircleJoinPlan(root, rel, best_path, tlist, caluses, custom_plans);


  /* Make the customScan plan node */
  // KInCircleJoinPlan * KInCircleJoinPlanNode = malloc(sizeof(KInCircleJoinPlanNode));
  // CustomScan * customscanNode = & KInCircleJoinPlanNode->customscanplan; 
  // Plan     *plan = &customscanNode->scan.plan;
  CustomScan * customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_plans = NIL;
  customscanNode->flags = best_path->flags;
  customscanNode->custom_scan_tlist = tlist;

  // KInCircleJoinPath * kInCircleJoin_best_path = (KInCircleJoinPath *)best_path;
  // customscanNode->custom_relids = bms_add_member(customscanNode->custom_relids , 
  //                                               kInCircleJoin_best_path->outerjoinpath->parent->relid );
  // customscanNode->custom_relids = bms_add_member(customscanNode->custom_relids , 
                                                // kInCircleJoin_best_path->innerjoinpath->parent->relid );
  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
  
  elog(NOTICE, "Plan_KInCircleJoinCustomPath ----------- 1 ");
  plan->lefttree = (Plan *)linitial(custom_plans); //???
  plan->righttree = (Plan *)lsecond(custom_plans); // ???

  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "KInCircleJoinCustomScanPlan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_KInCircleJoinCustomScan_state;
  customscanNode->methods = methods;

  elog(NOTICE, "Plan_KInCircleJoinCustomPath ----------- FINISH");

  return (Plan *) customscanNode;
}

Node *create_KInCircleJoinCustomScan_state(CustomScan *node)
{
  elog(NOTICE, "create_KInCircleJoinCustomScan_state ------------ start");
  
  // KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));
  // CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
  // CustomScanState
  /* TODO: new data Structure that contains the priority Queue to hold materialzed points */
  CustomScanState * KInCircleJoinCustomScanNode = palloc(sizeof(KInCircleJoinCustomScanNode));
  KInCircleJoinCustomScanNode->ss.ps.type = T_CustomScanState;
  /*
   * Set the basic state structure 
   */
  
  KInCircleJoinCustomScanNode->custom_ps = node->custom_plans; // these are not planstate yet
  KInCircleJoinCustomScanNode->pscan_len = 0;  

  
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  KInCircleJoinCustomScanNode->ss.ps.instrument = NULL;
  KInCircleJoinCustomScanNode->ss.ps.worker_instrument = NULL;
  KInCircleJoinCustomScanNode->ss.ps.chgParam = NULL;
  // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  KInCircleJoinCustomScanNode->ss.ps.lefttree = NULL;
  KInCircleJoinCustomScanNode->ss.ps.righttree = NULL; 
  KInCircleJoinCustomScanNode->ss.ps.initPlan = NULL;
  KInCircleJoinCustomScanNode->ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(30));
  name = "KInCircleJoinCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_KInCircleJoinCustomScan;
  methods->ExecCustomScan = Exec_KInCircleJoinCustomScan;
  methods->EndCustomScan = End_KInCircleJoinCustomScan;
  methods->ReScanCustomScan = ReScan_KInCircleJoinCustomScan;
  // methods->ExplainCustomScan = Explain_CacheCustomScan;
  
  KInCircleJoinCustomScanNode->methods = methods;

  /*
   * TODO: intialize the hash table 
   */

  elog(NOTICE, "create_KInCircleJoinCustomScan_state ------------ FINISH");  
  return (Node *) KInCircleJoinCustomScanNode;
}

void Begin_KInCircleJoinCustomScan (CustomScanState *node, EState *estate, int eflags)
{
  elog(NOTICE, "Begin_KInCircleJoinCustomScan ------------ start");

   PlanState * childplanstate = NULL;
   Plan * childPlan = NULL;
  
   /* initilaize the custom_exprs */
   CustomScan * plan = (CustomScan *)node->ss.ps.plan;
   node->ss.ps.qual = (List *)
    ExecInitExpr((Expr *) plan->custom_exprs,
           (PlanState *) node);

  if(list_length(node->custom_ps) == 2)
  {
    outerPlanState(node) = ExecInitNode(outerPlan(plan), estate, eflags);
    // if (node->nestParams == NIL)
    //   eflags |= EXEC_FLAG_REWIND;
    // else
    //   eflags &= ~EXEC_FLAG_REWIND;
    innerPlanState(node) = ExecInitNode(innerPlan(plan), estate, eflags);

    // childPlan = (Plan *)linitial(node->custom_ps);
    // childplanstate = ExecInitNode(childPlan, estate, eflags);

    // /* initialize children plan state */
    // if(childplanstate)
    // {
    //  node->custom_ps = NIL;
    //  node->custom_ps = lappend(node->custom_ps , childplanstate) ;

    //  /*
    //  * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
    //  */
    // ExecAssignScanType(&node->ss,
    //          ExecGetResultType(childplanstate));
    // }
  }
  



  /*
   * Initialize result tuple type and projection info.
   */
  // ExecAssignResultTypeFromTL(&node->ss.ps);
  // ExecAssignScanProjectionInfo(&node->ss);

  
  elog(NOTICE, "Begin_KInCircleJoinCustomScan ------------ FINISH");
}


TupleTableSlot *  Exec_KInCircleJoinCustomScan (CustomScanState *node)
{ 
  elog(NOTICE, "Exec_KInCircleJoinCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  // CacheCustomScanState * CacheNode = (CacheCustomScanState *) node;
  // IndexScanState * indexstate = &knnNode->indexstate;
  return ExecScan(&(node->ss),
          (ExecScanAccessMtd) MaterialCustomNext,
          (ExecScanRecheckMtd) MaterialCustomRecheck);
}

// static TupleTableSlot * MaterialCustomNext(CustomScanState *node)
// {
//   TupleTableSlot *slot = NULL;
//   // elog(NOTICE, "MaterialCustomNext ---------- start,   rows = %f" , node->ss.ps.plan->plan_rows);

//   if(list_length(node->custom_ps) == 1)
//     slot = ExecProcNode(linitial( node->custom_ps));
//   //else //TODO

//   // elog(NOTICE, "MaterialCustomNext ---------- finish,   rows = %f" , node->ss.ps.plan->plan_rows);
//   return slot;
// }

// static bool MaterialCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
// {
//   /* nothing to check */
//   return true; 
// }

void End_KInCircleJoinCustomScan (CustomScanState *node)
{
  elog(NOTICE, "End_KInCircleJoinCustomScan -------- start");
  
  /*
  * Free the exprcontext
  */
  ExecFreeExprContext(&node->ss.ps);

  /*
  * clean out the upper tuple table
  */
  ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
  * close down subquery
  */

  //End child plans
  // if(list_length(node->custom_ps) == 1)
    ExecEndNode(linitial(node->custom_ps));
    ExecEndNode(lsecond(node->custom_ps));

  // else //TODO
  elog(NOTICE, "End_KInCircleJoinCustomScan -------- finish");
}

void ReScan_KInCircleJoinCustomScan (CustomScanState *node)
{

  // elog(NOTICE, "ReScan_MaterialCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  ExecScanReScan(&node->ss);

  /*
  * ExecReScan doesn't know about my subplan, so I have to do
  * changed-parameter signaling myself.  This is just as well, because the
  * subplan has its own memory context in which its chgParam state lives.
  */
  PlanState * childplanstate =  (PlanState *)linitial( node->custom_ps);
  if (node->ss.ps.chgParam != NULL)
    UpdateChangedParamSet(childplanstate, node->ss.ps.chgParam);

  /*
  * if chgParam of subnode is not null then plan will be re-scanned by
  * first ExecProcNode.
  */
  if (childplanstate->chgParam == NULL)
    ExecReScan(childplanstate);

  // elog(NOTICE, "ReScan_MaterialCustomScan -------- Finish,   rows = %f" , node->ss.ps.plan->plan_rows);
}



//========================================================



// CustomPath * create_cacheCustomScan_path2(PlannerInfo *root, RelOptInfo *rel , Path * ParentPath)
// {
//   elog(NOTICE, "create_cacheCustomScan_path ----------- start , relid= %d " , rel->relid);
  
//   CustomPath     *pathnode = makeNode(CustomPath);
//   // Path * childpath  =NULL;

//   pathnode->path.pathtype = T_CustomScan;
  
//   // pathnode->path.parent = rel;
//   // pathnode->path.pathtarget = rel->reltarget;

//   // Assert(list_length( child_path) > 0);
  
//   if( IsA(parentPath, SubqueryScanPath) ) 
//   {
//     SubqueryScanPath * subquerypath = (SubqueryScanPath*) parentPath;
//     pathnode->path.parent = subquerypath->parent;
  
    
//     pathnode->custom_paths = lappend(pathnode->custom_paths, subquerypath->subpath);
//     subquerypath->subpath = pathnode;    
    
//     // elog(NOTICE, "create_cacheCustomScan_path ----------- 1 , child_path = %d " , childpath->pathtype);// ,nodeToString(childpath));
//     // pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
//     //                             required_outer);
//     pathnode->path.param_info = childpath->param_info; // TODO : our cache custom node should be identical like the child path

//     pathnode->path.pathtarget = childpath->pathtarget;
//     pathnode->path.parallel_aware = false;
//     pathnode->path.parallel_safe = rel->consider_parallel && childpath->parallel_safe;
//     pathnode->path.parallel_workers = childpath->parallel_workers;
//     pathnode->path.pathkeys = childpath->pathkeys;
    
//     // pathnode->custom_paths = lappend(pathnode->custom_paths, childpath);

//     /* compute the cost */
//     pathnode->path.rows = childpath->rows;     /* estimated number of result tuples */
//     pathnode->path.startup_cost = childpath->startup_cost; /* cost expended before fetching any tuples */
//     pathnode->path.total_cost = childpath->total_cost;   /* total cost (assuming all tuples fetched) */

//     /* set the rest of custom path attributes */
//     pathnode->custom_private = NIL;
    
//     struct CustomPathMethods * methods;
//     methods = palloc(sizeof(* methods));
//     char * name = palloc(sizeof(20));
//     name = "CacheCustomScan";
//     methods->CustomName = name;
//     methods->PlanCustomPath = Plan_CacheCustomPath2;
//     pathnode->methods = methods;

//     /* move any qualification from the subquery rel to the custom_exprs */
//     if(childpath->parent->baserestrictinfo )
//     {
//       elog(NOTICE, "create_cacheCustomScan_path ----------- 2 , move the baserestrictinfo to the CustomPAth");
//       // pathnode->custom_private =  childpath->parent->baserestrictinfo;
//       // childpath->parent->baserestrictinfo = NIL;
//       printf("\ncreate_cacheCustomScan_path ----------- 1 , rel:\n");
//       pprint(rel);
      
//     }

//     //DEBUG
//     printf("\ncreate_cacheCustomScan_path ============ childPath: \n");
//     pprint(childpath);

//     printf("create_cacheCustomScan_path ============= CustomPath:\n");
//     pprint(pathnode);
//     //DEBUG
    
//   }
//   else /* it a join path */
//   {}

//   return pathnode;
// }

CustomPath * replace_cacheCustomScan_path(PlannerInfo *root, RelOptInfo *rel , Path * currPath, List * child_path)
{
  elog(NOTICE, "create_cacheCustomScan_path ----------- start , relid= %d  , currPath->parent->relid=%d" , rel->relid, currPath->parent->relid);
  //DEBUG
  //DEBUG

  CustomPath     *pathnode = makeNode(CustomPath);
  // Path * childpath  =NULL;

  pathnode->path.pathtype = T_CustomScan;
  pathnode->path.parent = currPath->parent;
  pathnode->path.pathtarget = currPath->pathtarget;
  pathnode->flags = CUSTOMPATH_SUPPORT_MARK_RESTORE; // Do I need this ?? 

  Assert(list_length( child_path) > 0);
  
  if (list_length( child_path) == 1) /* it a SubqueryPath */
  {
    Assert(IsA( currPath , SubqueryScanPath));

    pathnode->custom_paths = lappend(pathnode->custom_paths, linitial(child_path));    
    
    pathnode->path.param_info = currPath->param_info; // TODO : our cache custom node should be identical like the child path

    pathnode->path.parallel_aware = false;
    pathnode->path.parallel_safe = currPath->parallel_safe;
    pathnode->path.parallel_workers = currPath->parallel_workers;
    pathnode->path.pathkeys = currPath->pathkeys;
    
    
    /* compute the cost */
    pathnode->path.rows = currPath->rows;     /* estimated number of result tuples */
    pathnode->path.startup_cost = currPath->startup_cost; /* cost expended before fetching any tuples */
    pathnode->path.total_cost = currPath->total_cost;   /* total cost (assuming all tuples fetched) */

    /* set the rest of custom path attributes */
    pathnode->custom_private = NIL;
    
    struct CustomPathMethods * methods;
    methods = palloc(sizeof(* methods));
    char * name = palloc(sizeof(20));
    name = "CacheCustomScan";
    methods->CustomName = name;
    methods->PlanCustomPath = Plan_CacheCustomPath;
    pathnode->methods = methods;

    //DEBUG
    printf("\ncreate_cacheCustomScan_path ============ childPath: \n");
    pprint(currPath);

    printf("create_cacheCustomScan_path ============= CustomPath:\n");
    pprint(pathnode);
    //DEBUG
  }
  else if (list_length( child_path) == 2) /* it a join path */
  {
    Path * outerpth = (Path *) linitial(child_path);
    Path * innerpth = (Path *) lsecond(child_path);
    
    pathnode->custom_paths = child_path;//lappend(pathnode->custom_paths, childpath);    
    
    pathnode->path.param_info = currPath->param_info; // TODO : our cache custom node should be identical like the child path

    pathnode->path.parallel_aware = false;
    pathnode->path.parallel_safe = currPath->parallel_safe;
    pathnode->path.parallel_workers = currPath->parallel_workers;
    pathnode->path.pathkeys = currPath->pathkeys;
    
    
    /* compute the cost */
    pathnode->path.rows = currPath->rows;     /* estimated number of result tuples */
    pathnode->path.startup_cost = currPath->startup_cost; /* cost expended before fetching any tuples */
    pathnode->path.total_cost = currPath->total_cost;   /* total cost (assuming all tuples fetched) */

    /* set the rest of custom path attributes */
    pathnode->custom_private = NIL;
    
    struct CustomPathMethods * methods;
    methods = palloc(sizeof(* methods));
    char * name = palloc(sizeof(20));
    name = "CacheCustomScan";
    methods->CustomName = name;
    methods->PlanCustomPath = Plan_CacheCustomPath; /* TODO: handle Join path planning */
    pathnode->methods = methods;

    //DEBUG
    printf("\ncreate_cacheCustomScan_path ============ childPath: \n");
    pprint(currPath);

    printf("create_cacheCustomScan_path ============= CustomPath:\n");
    pprint(pathnode);
    //DEBUG
    
  }

  return pathnode;
}


/* Plan for replace_cacheCustomScan_path */
static Plan * Plan_CacheCustomPath(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  elog(NOTICE, "Plan_CacheCustomPath ----------- start , relid= %d " , rel->relid);


  CustomScan * customscanNode; 
  
  /* Make the customScan plan node */
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  // plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_scan_tlist = tlist;  
  customscanNode->custom_plans = custom_plans;
  customscanNode->custom_exprs = clauses;
  customscanNode->flags = best_path->flags;

  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
 
  plan->lefttree = NULL; //custom_plans; ???
  plan->righttree = NULL; // ???

  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "CacheCustomScan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_CacheCustomScan_state;
  customscanNode->methods = methods;

  elog(NOTICE, "Plan_CacheCustomPath ----------- FINISH");

  return (Plan *) customscanNode;
}

static Plan * Plan_CacheCustomPath2(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist, List *clauses, List *custom_plans)
{
  elog(NOTICE, "Plan_CacheCustomPath2 ----------- start , relid= %d " , rel->relid);

  printf("\nPlan_CacheCustomPath2 ----------- 1, clauses:\n");
  pprint(clauses);


  CustomScan * customscanNode; 
  
  /* Make the customScan plan node */
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  plan->qual = clauses; // As planned, I will add any qual to the custom_exprs only

  customscanNode->custom_scan_tlist = tlist;  
  customscanNode->custom_plans = custom_plans;

  printf("\nPlan_CacheCustomPath2 ----------- 2, custom_plans:\n");
  pprint(custom_plans);

  
  // if(best_path->custom_private)
    customscanNode->custom_exprs = clauses;//best_path->custom_private;//list_concat(best_path->custom_private, clauses);
  // else
    // customscanNode->custom_exprs = clauses;

  customscanNode->flags = best_path->flags;

  /* set plan data from path */

  plan->startup_cost = best_path->path.startup_cost;
  plan->total_cost = best_path->path.total_cost;
  plan->plan_rows = best_path->path.rows;
  plan->plan_width = best_path->path.pathtarget->width;
  plan->parallel_aware = best_path->path.parallel_aware;
 
  plan->lefttree = NULL; //custom_plans; ???
  plan->righttree = NULL; // ???

  

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "CacheCustomScan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_CacheCustomScan_state2;
  customscanNode->methods = methods;

  elog(NOTICE, "Plan_CacheCustomPath2 ----------- FINISH");

  return (Plan *) customscanNode;
}

Node *create_CacheCustomScan_state(CustomScan *node)
{
  elog(NOTICE, "create_CacheCustomScan_state ------------ start");
  
  // KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));
  CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
  // CustomScanState
  
  /*
   * Set the basic state structure 
   */
  
  CacheCustomScanNode->base.custom_ps = node->custom_plans; // these are not planstate yet
  CacheCustomScanNode->base.pscan_len = 0;  

  CacheCustomScanNode->base.ss.ps.type = T_CustomScanState;
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  CacheCustomScanNode->base.ss.ps.instrument = NULL;
  CacheCustomScanNode->base.ss.ps.worker_instrument = NULL;
  CacheCustomScanNode->base.ss.ps.chgParam = NULL;
  // CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  //NULL;
  CacheCustomScanNode->base.ss.ps.lefttree = NULL;
  CacheCustomScanNode->base.ss.ps.righttree = NULL; 
  CacheCustomScanNode->base.ss.ps.initPlan = NULL;
  CacheCustomScanNode->base.ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(30));
  name = "CacheCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_CacheCustomScan;
  methods->ExecCustomScan = Exec_CacheCustomScan;
  methods->EndCustomScan = End_CacheCustomScan;
  methods->ReScanCustomScan = ReScan_CacheCustomScan;
  // methods->ExplainCustomScan = Explain_CacheCustomScan;
  
  CacheCustomScanNode->base.methods = methods;

  /*
   * TODO: intialize the hash table 
   */

  elog(NOTICE, "create_CacheCustomScan_state ------------ FINISH");  
  return (Node *) CacheCustomScanNode;
}

Node *create_CacheCustomScan_state2(CustomScan *node)
{
  elog(NOTICE, "create_CacheCustomScan_state2 ------------ start");
  
  // KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));
  CacheCustomScanState * CacheCustomScanNode = palloc(sizeof(CacheCustomScanState));
  // CustomScanState
  
  /*
   * Set the basic state structure 
   */
  
  CacheCustomScanNode->base.custom_ps = node->custom_plans; // these are not planstate yet
  CacheCustomScanNode->base.pscan_len = 0;  

  CacheCustomScanNode->base.ss.ps.type = T_CustomScanState;
  
  // CacheCustomScanNode->base.ss.ps.plan = (Plan *) node;
  CacheCustomScanNode->base.ss.ps.instrument = NULL;
  CacheCustomScanNode->base.ss.ps.worker_instrument = NULL;
  CacheCustomScanNode->base.ss.ps.chgParam = NULL;
  CacheCustomScanNode->base.ss.ps.qual = node->custom_exprs;  // TODO: shoould be handeled manually
  CacheCustomScanNode->base.ss.ps.lefttree = NULL;
  CacheCustomScanNode->base.ss.ps.righttree = NULL; 
  CacheCustomScanNode->base.ss.ps.initPlan = NULL;
  CacheCustomScanNode->base.ss.ps.subPlan = NULL;
  
  /*
   * Set the custom state structure 
   */
  
  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(30));
  name = "CacheCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_CacheCustomScan;
  methods->ExecCustomScan = Exec_CacheCustomScan;
  methods->EndCustomScan = End_CacheCustomScan;
  methods->ReScanCustomScan = ReScan_CacheCustomScan;
  // methods->ExplainCustomScan = Explain_CacheCustomScan;
  
  CacheCustomScanNode->base.methods = methods;

  /*
   * TODO: intialize the hash table 
   */

  elog(NOTICE, "create_CacheCustomScan_state2 ------------ FINISH");  
  return (Node *) CacheCustomScanNode;
}

void Begin_CacheCustomScan (CustomScanState *node, EState *estate, int eflags)
{
  elog(NOTICE, "Begin_CacheCustomScan ------------ start");

   PlanState * childplanstate = NULL;
   Plan * childPlan = NULL;
  
   /* initilaize the custom_exprs */
   CustomScan * plan = (CustomScan *)node->ss.ps.plan;
   node->ss.ps.qual = (List *)
    ExecInitExpr((Expr *) plan->custom_exprs,
           (PlanState *) node);

  if(list_length(node->custom_ps) == 1)
  {
    childPlan = (Plan *)linitial(node->custom_ps);
    childplanstate = ExecInitNode(childPlan, estate, eflags);

    /* initialize children plan state */
    if(childplanstate)
    {
     node->custom_ps = NIL;
     node->custom_ps = lappend(node->custom_ps , childplanstate) ;

     /*
     * Initialize scan tuple type (needed by ExecAssignScanProjectionInfo)
     */
    ExecAssignScanType(&node->ss,
             ExecGetResultType(childplanstate));
    }
  }
  // else //TODO



  /*
   * Initialize result tuple type and projection info.
   */
  ExecAssignResultTypeFromTL(&node->ss.ps);
  ExecAssignScanProjectionInfo(&node->ss);

  
  elog(NOTICE, "Begin_CacheCustomScan ------------ FINISH");
}


TupleTableSlot *  Exec_CacheCustomScan (CustomScanState *node)
{ 
  elog(NOTICE, "Exec_CacheCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  CacheCustomScanState * CacheNode = (CacheCustomScanState *) node;
  // IndexScanState * indexstate = &knnNode->indexstate;
  return ExecScan(&(CacheNode->base.ss),
          (ExecScanAccessMtd) CacheCustomNext,
          (ExecScanRecheckMtd) CacheCustomRecheck);
}

static TupleTableSlot * CacheCustomNext(CustomScanState *node)
{
  TupleTableSlot *slot = NULL;
  // elog(NOTICE, "CacheCustomNext ---------- start,   rows = %f" , node->ss.ps.plan->plan_rows);

  if(list_length(node->custom_ps) == 1)
    slot = ExecProcNode(linitial( node->custom_ps));
  //else //TODO

  // elog(NOTICE, "CacheCustomNext ---------- finish,   rows = %f" , node->ss.ps.plan->plan_rows);
  return slot;
}

static bool CacheCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true; 
}

void End_CacheCustomScan (CustomScanState *node)
{
   elog(NOTICE, "End_CacheCustomScan -------- start");
  
   /*
   * Free the exprcontext
   */
  ExecFreeExprContext(&node->ss.ps);

  /*
   * clean out the upper tuple table
   */
  ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
   * close down subquery
   */
  
  //End child plans
  if(list_length(node->custom_ps) == 1)
    ExecEndNode(linitial(node->custom_ps));
  // else //TODO
  elog(NOTICE, "End_CacheCustomScan -------- finish");
}

void ReScan_CacheCustomScan (CustomScanState *node)
{

  // elog(NOTICE, "ReScan_CacheCustomScan -------- start,   rows = %f" , node->ss.ps.plan->plan_rows);
  ExecScanReScan(&node->ss);

  /*
   * ExecReScan doesn't know about my subplan, so I have to do
   * changed-parameter signaling myself.  This is just as well, because the
   * subplan has its own memory context in which its chgParam state lives.
   */
  PlanState * childplanstate =  (PlanState *)linitial( node->custom_ps);
  if (node->ss.ps.chgParam != NULL)
    UpdateChangedParamSet(childplanstate, node->ss.ps.chgParam);

  /*
   * if chgParam of subnode is not null then plan will be re-scanned by
   * first ExecProcNode.
   */
  if (childplanstate->chgParam == NULL)
    ExecReScan(childplanstate);

  // elog(NOTICE, "ReScan_CacheCustomScan -------- Finish,   rows = %f" , node->ss.ps.plan->plan_rows);
}

// static void Explain_CacheCustomScan(CustomScanState *node, List *ancestors, ExplainState *es)
// {
//   CacheCustomScanState  *ctss = (CacheCustomScanState *) node;
//   CustomScan     *cscan = (CustomScan *) ctss->base.css.ss.ps.plan;

//   /* logic copied from show_qual and show_expression */
//   if (cscan->custom_exprs)
//   {
//     bool  useprefix = es->verbose;
//     Node   *qual;
//     List   *context;
//     char   *exprstr;

//     /* Convert AND list to explicit AND */
//     qual = (Node *) make_ands_explicit(cscan->custom_exprs);

//     /* Set up deparsing context */
//     context = set_deparse_context_planstate(es->deparse_cxt,
//                         (Node *) &node->ss.ps,
//                                                 ancestors);

//     /* Deparse the expression */
//     exprstr = deparse_expression(qual, context, useprefix, false);

//     /* And add to es->str */
//     ExplainPropertyText("Custom quals", exprstr, es);
//   }
// }
/*****************************************************************************
 *  Helper Function for planning
 ****************************************************************************/

/*
 * subquery_push_qual - push down a qual that we have determined is safe
 */
 void
my_subquery_push_qual(Query *subquery, RangeTblEntry *rte, Index rti, Node *qual)
{
  if (subquery->setOperations != NULL)
  {
    /* Recurse to push it separately to each component query */
    my_recurse_push_qual(subquery->setOperations, subquery,
              rte, rti, qual);
  }
  else if (IsA(qual, CurrentOfExpr))
  {
    /*
     * This is possible when a WHERE CURRENT OF expression is applied to a
     * table with row-level security.  In that case, the subquery should
     * contain precisely one rtable entry for the table, and we can safely
     * push the expression down into the subquery.  This will cause a TID
     * scan subquery plan to be generated allowing the target relation to
     * be updated.
     *
     * Someday we might also be able to use a WHERE CURRENT OF expression
     * on a view, but currently the rewriter prevents that, so we should
     * never see any other case here, but generate sane error messages in
     * case it does somehow happen.
     */
    if (subquery->rtable == NIL)
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("WHERE CURRENT OF is not supported on a view with no underlying relation")));

    if (list_length(subquery->rtable) > 1)
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("WHERE CURRENT OF is not supported on a view with more than one underlying relation")));

    if (subquery->hasAggs || subquery->groupClause || subquery->groupingSets || subquery->havingQual)
      ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("WHERE CURRENT OF is not supported on a view with grouping or aggregation")));

    /*
     * Adjust the CURRENT OF expression to refer to the underlying table
     * in the subquery, and attach it to the subquery's WHERE clause.
     */
    qual = copyObject(qual);
    ((CurrentOfExpr *) qual)->cvarno = 1;

    subquery->jointree->quals =
      make_and_qual(subquery->jointree->quals, qual);
  }
  else
  {
    /*
     * We need to replace Vars in the qual (which must refer to outputs of
     * the subquery) with copies of the subquery's targetlist expressions.
     * Note that at this point, any uplevel Vars in the qual should have
     * been replaced with Params, so they need no work.
     *
     * This step also ensures that when we are pushing into a setop tree,
     * each component query gets its own copy of the qual.
     */
    qual = ReplaceVarsFromTargetList(qual, rti, 0, rte,
                     subquery->targetList,
                     REPLACEVARS_REPORT_ERROR, 0,
                     &subquery->hasSubLinks);

    /*
     * Now attach the qual to the proper place: normally WHERE, but if the
     * subquery uses grouping or aggregation, put it in HAVING (since the
     * qual really refers to the group-result rows).
     */
    if (subquery->hasAggs || subquery->groupClause || subquery->groupingSets || subquery->havingQual)
      subquery->havingQual = make_and_qual(subquery->havingQual, qual);
    else
      subquery->jointree->quals =
        make_and_qual(subquery->jointree->quals, qual);

    /*
     * We need not change the subquery's hasAggs or hasSublinks flags,
     * since we can't be pushing down any aggregates that weren't there
     * before, and we don't push down subselects at all.
     */
  }
}

/*
 * Helper routine to recurse through setOperations tree
 */
 void
my_recurse_push_qual(Node *setOp, Query *topquery,
          RangeTblEntry *rte, Index rti, Node *qual)
{
  elog(NOTICE, "my_recurse_push_qual  ... start");
  if (IsA(setOp, RangeTblRef))
  {
    RangeTblRef *rtr = (RangeTblRef *) setOp;
    RangeTblEntry *subrte = rt_fetch(rtr->rtindex, topquery->rtable);
    Query    *subquery = subrte->subquery;

    Assert(subquery != NULL);
    my_subquery_push_qual(subquery, rte, rti, qual);
  }
  else if (IsA(setOp, SetOperationStmt))
  {
    SetOperationStmt *op = (SetOperationStmt *) setOp;

    my_recurse_push_qual(op->larg, topquery, rte, rti, qual);
    my_recurse_push_qual(op->rarg, topquery, rte, rti, qual);
  }
  else
  {
    elog(ERROR, "my_recurse_push_qual: unrecognized node type: %d",
       (int) nodeTag(setOp));
  }
}

/*
 * remove_unused_subquery_outputs
 *    Remove subquery targetlist items we don't need
 *
 * It's possible, even likely, that the upper query does not read all the
 * output columns of the subquery.  We can remove any such outputs that are
 * not needed by the subquery itself (e.g., as sort/group columns) and do not
 * affect semantics otherwise (e.g., volatile functions can't be removed).
 * This is useful not only because we might be able to remove expensive-to-
 * compute expressions, but because deletion of output columns might allow
 * optimizations such as join removal to occur within the subquery.
 *
 * To avoid affecting column numbering in the targetlist, we don't physically
 * remove unused tlist entries, but rather replace their expressions with NULL
 * constants.  This is implemented by modifying subquery->targetList.
 */
void
my_remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel)
{
  Bitmapset  *attrs_used = NULL;
  ListCell   *lc;

  /*
   * Do nothing if subquery has UNION/INTERSECT/EXCEPT: in principle we
   * could update all the child SELECTs' tlists, but it seems not worth the
   * trouble presently.
   */
  if (subquery->setOperations)
    return;

  /*
   * If subquery has regular DISTINCT (not DISTINCT ON), we're wasting our
   * time: all its output columns must be used in the distinctClause.
   */
  if (subquery->distinctClause && !subquery->hasDistinctOn)
    return;

  /*
   * Collect a bitmap of all the output column numbers used by the upper
   * query.
   *
   * Add all the attributes needed for joins or final output.  Note: we must
   * look at rel's targetlist, not the attr_needed data, because attr_needed
   * isn't computed for inheritance child rels, cf set_append_rel_size().
   * (XXX might be worth changing that sometime.)
   */
  pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

  /* Add all the attributes used by un-pushed-down restriction clauses. */
  foreach(lc, rel->baserestrictinfo)
  {
    RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

    pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
  }

  /*
   * If there's a whole-row reference to the subquery, we can't remove
   * anything.
   */
  if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used))
    return;

  /*
   * Run through the tlist and zap entries we don't need.  It's okay to
   * modify the tlist items in-place because set_subquery_pathlist made a
   * copy of the subquery.
   */
  foreach(lc, subquery->targetList)
  {
    TargetEntry *tle = (TargetEntry *) lfirst(lc);
    Node     *texpr = (Node *) tle->expr;

    /*
     * If it has a sortgroupref number, it's used in some sort/group
     * clause so we'd better not remove it.  Also, don't remove any
     * resjunk columns, since their reason for being has nothing to do
     * with anybody reading the subquery's output.  (It's likely that
     * resjunk columns in a sub-SELECT would always have ressortgroupref
     * set, but even if they don't, it seems imprudent to remove them.)
     */
    if (tle->ressortgroupref || tle->resjunk)
      continue;

    /*
     * If it's used by the upper query, we can't remove it.
     */
    if (bms_is_member(tle->resno - FirstLowInvalidHeapAttributeNumber,
              attrs_used))
      continue;

    /*
     * If it contains a set-returning function, we can't remove it since
     * that could change the number of rows returned by the subquery.
     */
    if (expression_returns_set(texpr))
      continue;

    /*
     * If it contains volatile functions, we daren't remove it for fear
     * that the user is expecting their side-effects to happen.
     */
    if (contain_volatile_functions(texpr))
      continue;

    /*
     * OK, we don't need it.  Replace the expression with a NULL constant.
     * Preserve the exposed type of the expression, in case something
     * looks at the rowtype of the subquery's result.
     */
    tle->expr = (Expr *) makeNullConst(exprType(texpr),
                       exprTypmod(texpr),
                       exprCollation(texpr));
  }
}


/*
 * create_RInCircle_path
 *    Creates a path corresponding to a K_in_circle scan, returning the
 *    pathnode.
 */
CustomPath *
create_KInCircle_path(PlannerInfo *root, RelOptInfo *rel,
          Relids required_outer, OpExpr* KNN_op , 
          int k  ,Path *subpath)
{
  elog(NOTICE, "create_KInCircle_path :   start");
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  pathnode->path.parent = rel;
  pathnode->path.pathtarget = rel->reltarget;
  //pathnode->path.param_info = get_baserel_parampathinfo(root, rel, required_outer);
  pathnode->path.parallel_aware =  false;
  pathnode->path.parallel_safe = false;
  pathnode->path.parallel_workers = subpath->parallel_workers;
  pathnode->path.pathkeys = NIL;
  pathnode->custom_paths = lcons(subpath , pathnode->custom_paths); 
  
  pathnode->path.rows = k; // KInCircle algorithm return  k nearest tuples (estimated rows = k rows)
  

  //cost_KInCircleScan(pathnode, root, rel, pathnode->path.param_info, k);
  pathnode->path.startup_cost = subpath->startup_cost;
  pathnode->path.total_cost = subpath->total_cost;

  Cost startup_cost = 0.0;
  Cost run_cost = 0.0;

  startup_cost += pathnode->path.pathtarget->cost.startup;
  run_cost += log(k) * subpath->rows;

  pathnode->path.startup_cost += startup_cost;
  pathnode->path.total_cost += startup_cost + run_cost;

  
  elog(NOTICE, "create_KInCircle_path :   2");
  
  // set private data needed in KInCircle algorithm 
  KInCircle_data * e = palloc(sizeof (* e));
  e->k = k;
  e->indexscanNode.indexorderbyorig = NIL;
  e->indexscanNode.indexorderbyorig = lappend(e->indexscanNode.indexorderbyorig , KNN_op) ;//copyObject(KNN_op);
  pathnode->custom_private = lcons(e, pathnode->custom_private);
  

  struct CustomPathMethods * methods;// = palloc(sizeof(* methods));
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "KInCircleScan_path";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_KInCirclePath;
  elog(NOTICE, "create_KInCircle_path :   5");
  pathnode->methods = methods;


  //^_^ to handle set_plan_references for the subroot 
  //root->glob->subroots = lappend( root->glob->subroots,  rel->subroot);
  elog(NOTICE, "create_KInCircle_path :   6");
  return pathnode;
}

CustomPath *
create_KInCircle_path2(PlannerInfo *root, RelOptInfo *rel,
          Relids required_outer, OpExpr* KNN_op , 
          int k  ,Path *subpath)
{
  elog(NOTICE, "create_KInCircle_path :   start");
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  pathnode->path.parent = rel;
  pathnode->path.pathtarget = rel->reltarget;
  //pathnode->path.param_info = get_baserel_parampathinfo(root, rel, required_outer);
  pathnode->path.parallel_aware =  false;
  pathnode->path.parallel_safe = false;
  pathnode->path.parallel_workers = subpath->parallel_workers;
  pathnode->path.pathkeys = NIL;
  pathnode->custom_paths = lcons(subpath , pathnode->custom_paths); 
  
  pathnode->path.rows = k; // KInCircle algorithm return  k nearest tuples (estimated rows = k rows)
  

  //cost_KInCircleScan(pathnode, root, rel, pathnode->path.param_info, k);
  pathnode->path.startup_cost = subpath->startup_cost;
  pathnode->path.total_cost = subpath->total_cost;

  Cost startup_cost = 0.0;
  Cost run_cost = 0.0;

  startup_cost += pathnode->path.pathtarget->cost.startup;
  run_cost += log(k) * subpath->rows;

  pathnode->path.startup_cost += startup_cost;
  pathnode->path.total_cost += startup_cost + run_cost;

  
  elog(NOTICE, "create_KInCircle_path :   2");
  
  // set private data needed in KInCircle algorithm 
  KInCircle_data * e = palloc(sizeof (* e));
  e->k = k;
  e->indexscanNode.indexorderbyorig = NIL;
  e->indexscanNode.indexorderbyorig = lappend(e->indexscanNode.indexorderbyorig , KNN_op) ;//copyObject(KNN_op);
  pathnode->custom_private = lcons(e, pathnode->custom_private);
  

  struct CustomPathMethods * methods;// = palloc(sizeof(* methods));
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "KInCircleScan_path";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_KInCirclePath;
  elog(NOTICE, "create_KInCircle_path :   5");
  pathnode->methods = methods;


  //^_^ to handle set_plan_references for the subroot 
  //root->glob->subroots = lappend( root->glob->subroots,  rel->subroot);
  elog(NOTICE, "create_KInCircle_path :   6");
  return pathnode;
}
/*
 * create_RInCircle_path
 *    Creates a path corresponding to an Ordered Index Scan for quad spgist Index
 */
CustomPath *
create_basicCustomScan_path(PlannerInfo *root, RelOptInfo *rel , Path * child_path, OpExpr* KNN_op, int k)
{
  // elog(NOTICE, "create_basicCustomScan_path :   start");
  
  
  CustomPath     *pathnode = makeNode(CustomPath);
  pathnode->path.pathtype = T_CustomScan;
  pathnode->path.parent = rel;
  pathnode->path.pathtarget = rel->reltarget;
  //pathnode->path.param_info = get_baserel_parampathinfo(root, rel, required_outer);
  pathnode->path.parallel_aware =  false;
  pathnode->path.parallel_safe = false;
  pathnode->path.parallel_workers = 0;//child_path->parallel_workers;
  pathnode->path.pathkeys = NULL;
  pathnode->custom_paths = NULL;//lcons(child_path , pathnode->custom_paths); 
  
  // set private data needed in KInCircle algorithm 
  KInCircle_data * e = palloc(sizeof (* e));
  e->k = k;
  //e->opr = copyObject(KNN_op);
  e->indexscanNode.indexorderby = NIL;
    e->indexscanNode.indexorderbyorig = NIL;
  e->indexscanNode.indexorderby = lappend(e->indexscanNode.indexorderby, KNN_op);
  e->indexscanNode.indexorderbyorig =lappend(e->indexscanNode.indexorderbyorig, KNN_op);
  pathnode->custom_private = NIL;
  pathnode->custom_private = lcons(e, pathnode->custom_private);
    
  //TODO : this should be changed
  pathnode->path.rows = k;//child_path->rows; 
  pathnode->path.startup_cost = 0; //child_path->startup_cost;
  pathnode->path.total_cost = 0 ; //child_path->total_cost; //TODO: I need to know if I should consider qual cost 

  struct CustomPathMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "BasicCustomScan";
  methods->CustomName = name;
  methods->PlanCustomPath = Plan_BasicCustomPath;
  pathnode->methods = methods;
  
  return pathnode;
}



static Plan * Plan_KInCirclePath(PlannerInfo *root,
                        RelOptInfo *rel,
                        struct CustomPath *best_path,
                        List *tlist,
                        List *clauses,
                        List *custom_plans)
{
  elog(NOTICE, "Plan_KInCirclePath :   1");
  
  Index   scan_relid = rel->relid;
  CustomScan * customscanNode; 
  
  elog(NOTICE, "Plan_KInCirclePath :   2");
 

  /* Make the customScan plan node */
  customscanNode = make_customScan(tlist,
                                  clauses,
                                  scan_relid,
                                  custom_plans,
                                  best_path->custom_private);
 


  //^_^ to handle set_plan_references for the subroot 
  root->glob->subplans = lappend( root->glob->subplans,  linitial(custom_plans));

  elog(NOTICE, "Plan_KInCirclePath :   4");
  return (Plan *) customscanNode;
}


static Plan * Plan_BasicCustomPath(PlannerInfo *root,
                        RelOptInfo *rel,
                        struct CustomPath *best_path,
                        List *tlist,
                        List *clauses,
                        List *custom_plans)
{
  // elog(NOTICE, "Plan_BasicCustomPath : .........   start");
  // printf("\nPlan_BasicCustomPath================== rel\n");
  // pprint(rel);
  Index   scanrelid = rel->relid;
  CustomScan * customscanNode; 
  KInCircle_data *e;
  AttrNumber varattno; // for the order by operator used in the query
  /* Make the customScan plan node */
 
  customscanNode = makeNode(CustomScan);
  Plan     *plan = &customscanNode->scan.plan;

  plan->type = T_CustomScan; 
  plan->targetlist = tlist;
  customscanNode->custom_scan_tlist = NULL;//tlist; // I think it's needed so set_plan_references are needed (without it I have errors variable not found in subplan targetlist)
  
  RestrictInfo *r ;
  if(clauses)
  {
    r = linitial(clauses);
    Expr *expr = r->clause;
    plan->qual = lappend(plan->qual , expr); // for now only the relational predicate (qual) from parent query is saved in custom_private
  }
  
  plan->lefttree = NULL; //custom_plans;
  plan->righttree = NULL;

  //Find the Index relation that the operator ORDER BY is using

  // get the order by attno used in the query 
  e = linitial(best_path->custom_private); // assuming we have only one element in the private list of type KInCircle_data
  OpExpr * opr = linitial(e->indexscanNode.indexorderbyorig);
  if( IsA(linitial(opr->args) , Var) )
  {
    Var * var = linitial(opr->args);
    varattno = var->varattno;
  }
  else if( IsA(lsecond(opr->args) , Var) )
  {
    Var * var = lsecond(opr->args);
    varattno = var->varattno;
  }
  // find the corresponding index relation
  ListCell *l, *lc;
  foreach(l, rel->indexlist)
  {
    IndexOptInfo * index = lfirst(l);
    foreach(lc , index->indextlist)
    {
      TargetEntry * te = lfirst(lc);
      Expr * expr = te->expr;
      Var * var = (Var *)expr;
      if(var->varattno == varattno) // this is the index that is used in the quey ordery bu clause
      {
        e->indexscanNode.indexid = index->indexoid;
        break;
      }
    }
  }

  //set the ordery bu operation in the custom_exprs field in the plan node
  // OpExpr *op = copyObject(e->opr);
  customscanNode->custom_exprs = lcons(opr , customscanNode->custom_exprs);
  
  // add the index relation OID to the private data 

  customscanNode->custom_private = list_copy(best_path->custom_private);

  customscanNode->scan.scanrelid = scanrelid;
  
  customscanNode->custom_plans = custom_plans;

  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "BasicCustomScan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_BasicCustomScan_state;
  customscanNode->methods = methods;

  // elog(NOTICE, "Plan_BasicCustomPath : ...........   end");
  // printf("\nPlan_BasicCustomPath================== clauses\n");
  // pprint(clauses);
  return (Plan *) customscanNode;
}


static CustomScan * make_customScan(List *tlist,
                                    List *qual,
                                    Index scanrelid,
                                    List * custom_plans,
                                    List * custom_private)
{
  CustomScan *node = makeNode(CustomScan);
  Plan     *plan = &node->scan.plan;

  plan->type = T_CustomScan; // ????
  plan->targetlist = tlist;
  plan->qual = qual;
  plan->lefttree = NULL;
  plan->righttree = NULL;
  node->scan.scanrelid = scanrelid;
  node->custom_scan_tlist = tlist;
  node->custom_plans = custom_plans;

  // ListCell * l;
  // foreach(l, custom_private)
  // {
  //   KInCircle_data *e = lfirst(l);
  //   if(e)
  //   {
  //     node->custom_exprs = lcons(e->opr , node->custom_exprs);
  //     //plan->qual = lcons(e->opr, plan->qual);
  //   }
  // }
  node->custom_private = list_copy(custom_private);
  
  struct CustomScanMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "KInCircleScan_plan";
  methods->CustomName = name;
  methods->CreateCustomScanState = create_KInCircleScan_state;
  node->methods = methods;
  return node;
}

/* create_customscan_state*/
Node *create_KInCircleScan_state(CustomScan *node)
{
  elog(NOTICE, "create_KInCircleScan_state ....... start");

  CustomScanState * customScanStateNode;

  /* SubqueryScan should not have any "normal" children */
  Assert(outerPlan(node) == NULL);
  Assert(innerPlan(node) == NULL);
  /*
   * create state structure
   */
  customScanStateNode = makeNode(CustomScanState);
  customScanStateNode->ss.ps.type = T_CustomScanState;
 
  customScanStateNode->custom_ps = node->custom_plans;

  struct CustomExecMethods * methods;
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "KInCircleScan_state";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_KInCircleScan;
  methods->ExecCustomScan = Exec_KInCircleScan;
  methods->EndCustomScan = End_KInCircleScan;
  methods->ReScanCustomScan = ReScan_KInCircleScan;

  customScanStateNode->methods = methods;

  // elog(NOTICE, "create_KInCircleScan_state ....... end");
  return (Node *) customScanStateNode;
}

Node * create_BasicCustomScan_state(CustomScan *node)
{
  // elog(NOTICE, "create_BasicCustomScan_state ....... start");
  // elog(NOTICE, "size KInCircleState = %lu ,  size CustomScanState = %lu", sizeof(KInCircleState) , sizeof(CustomScanState));
  KInCircleState * KInCircleNode =  palloc(sizeof(KInCircleState));

  Assert(outerPlan(node) == NULL);
  Assert(innerPlan(node) == NULL);
  /*
   * create state structure 
   */

  KInCircleNode->base.ss.ps.type = T_CustomScanState;
  KInCircleNode->base.custom_ps = node->custom_plans;
  KInCircleNode->base.pscan_len = 0;
  KInCircleNode->base.ss.ps.plan = (Plan *) node;
  KInCircleNode->base.ss.ps.instrument = NULL;
  KInCircleNode->base.ss.ps.worker_instrument = NULL;
  KInCircleNode->base.ss.ps.chgParam = NULL;
  KInCircleNode->base.ss.ps.qual = NULL;
  KInCircleNode->base.ss.ps.lefttree = NULL;
  KInCircleNode->base.ss.ps.righttree = NULL; 
  KInCircleNode->base.ss.ps.initPlan = NULL;
  KInCircleNode->base.ss.ps.subPlan = NULL;
  // KInCircleNode->base.ss.ps.
  // KInCircleNode->base.ss.ps.

  struct CustomExecMethods * methods;// = palloc(sizeof(* methods));
  methods = palloc(sizeof(* methods));
  char * name = palloc(sizeof(20));
  name = "BasicCustomScanState";
  methods->CustomName = name;
  methods->BeginCustomScan = Begin_BasicCustomScan;
  methods->ExecCustomScan = Exec_BasicCustomScan;
  methods->EndCustomScan = End_BasicCustomScan;
  methods->ReScanCustomScan = ReScan_BasicCustomScan;

  //customScanStateNode->methods = methods;
  KInCircleNode->base.methods = methods;

  /* set another fields required 
     for the KInCircle 
  */

  // set the order by operator
  KInCircleNode->indexstate.ss.ps.type = T_IndexScanState;
  KInCircleNode->indexstate.indexorderbyorig = NIL;
  
  //OpExpr * op = linitial(node->custom_exprs);
  // KInCircleNode->indexstate.indexorderbyorig = lappend( KInCircleNode->indexstate.indexorderbyorig,  op);

  //set the indexid used
  KInCircle_data * e = linitial(node->custom_private);
  OpExpr * opr = linitial(e->indexscanNode.indexorderbyorig);
  KInCircleNode->indexid = e->indexscanNode.indexid;
  KInCircleNode->indexstate.indexorderbyorig = lappend( KInCircleNode->indexstate.indexorderbyorig, opr);
  // elog(NOTICE, "create_BasicCustomScan_state ....... end");
  return (Node *) KInCircleNode;
}

void Begin_KInCircleScan (CustomScanState *node, EState *estate, int eflags)
{
  // elog(NOTICE , "Begin_KInCircleScan ========= start");
  
  // Plan * child_plan = linitial(node->custom_ps);
  // PlanState * child_ps = ExecInitNode(child_plan , estate, eflags);
  node->custom_ps = NIL;
  // node->custom_ps = lcons(child_ps , node->custom_ps );
  node->custom_ps = lcons(linitial(estate->es_subplanstates) ,node->custom_ps) ;

  // elog(NOTICE , "Begin_KInCircleScan ========= end");
 
}


void Begin_BasicCustomScan (CustomScanState *node, EState *estate, int eflags)
{}
// {  
//   // elog(NOTICE , "Begin_BasicCustomScan ========= start");
  
//   Relation  currentRelation = node->ss.ss_currentRelation;
//   // bool    relistarget;
//   KInCircleState * p_node = (KInCircleState *) node;
//   IndexScanState * indexstate = &p_node->indexstate;
//   List* OrderByOpr = NIL;
//   OrderByOpr =lappend(OrderByOpr, linitial(p_node->indexstate.indexorderbyorig));
//   //initialize the order by operation
  
//   p_node->indexstate.indexorderbyorig = (List *)
//     ExecInitExpr((Expr *) (p_node->indexstate.indexorderbyorig), (PlanState *) p_node);

//   // elog(NOTICE , "Begin_BasicCustomScan ========= 1");  
 
//   /* nodeCustom.c opens the relation and get the scan type from the relation descriptor */

//   /*
//    * Open the index relation.
//    *
//    * If the parent table is one of the target relations of the query, then
//    * InitPlan already opened and write-locked the index, so we can avoid
//    * taking another lock here.  Otherwise we need a normal reader's lock.
//    */
//   //relistarget = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
//   p_node->indexstate.iss_RelationDesc = index_open(p_node->indexid,
//                                        AccessShareLock);

//   // elog(NOTICE , "Begin_BasicCustomScan ========= 2"); 

//   /*
//    * any ORDER BY exprs have to be turned into scankeys in the same way
//    * TODO: this one should be called when we create an operator for the 
//            order by and add it in the catalog table 
//    */
//   indexstate->iss_NumScanKeys = 0;
//   indexstate->iss_ScanKeys = NULL;
//   indexstate->iss_RuntimeKeys = NULL;
//   indexstate->iss_NumRuntimeKeys = 0;
//   indexstate->iss_ReachedEnd = false;

//   my_ExecIndexBuildScanKeys((PlanState *) p_node,
//                indexstate->iss_RelationDesc,
//                OrderByOpr,
//                true,
//                &indexstate->iss_OrderByKeys,
//                &indexstate->iss_NumOrderByKeys,
//                &indexstate->iss_RuntimeKeys,
//                &indexstate->iss_NumRuntimeKeys,
//                NULL,  /* no ArrayKeys */
//                NULL);

 

//   /* Initialize sort support, if we need to re-check ORDER BY exprs */
//   if (indexstate->iss_NumOrderByKeys > 0)
//   {
//     int     numOrderByKeys = indexstate->iss_NumOrderByKeys;
//     int     i;
//     // ListCell   *lco;
//     ListCell   *lcx;

//     /*
//      * Prepare sort support, and look up the data type for each ORDER BY
//      * expression.
//      */
//     //Assert(numOrderByKeys == list_length(node->indexorderbyops));
//     //Assert(numOrderByKeys == list_length(node->indexorderbyorig));
//     indexstate->iss_SortSupport = (SortSupportData *)
//       palloc0(numOrderByKeys * sizeof(SortSupportData));
//     indexstate->iss_OrderByTypByVals = (bool *)
//       palloc(numOrderByKeys * sizeof(bool));
//     indexstate->iss_OrderByTypLens = (int16 *)
//       palloc(numOrderByKeys * sizeof(int16));

//     i = 0;    
//     foreach( lcx, OrderByOpr)
//     {
//       Oid     orderbyop = 672;//lfirst_oid(lco);
//       Node     *orderbyexpr = (Node *) lfirst(lcx);
//       Oid     orderbyType = exprType(orderbyexpr);
//       Oid     orderbyColl = exprCollation(orderbyexpr);
//       SortSupport orderbysort = &indexstate->iss_SortSupport[i];

//       /* Initialize sort support */
//       orderbysort->ssup_cxt = CurrentMemoryContext;
//       orderbysort->ssup_collation = orderbyColl;
//       /* See cmp_orderbyvals() comments on NULLS LAST */
//       orderbysort->ssup_nulls_first = false;
//       /* ssup_attno is unused here and elsewhere */
//       orderbysort->ssup_attno = 0;
//       /* No abbreviation */
//       orderbysort->abbreviate = false;
//       PrepareSortSupportFromOrderingOp(orderbyop, orderbysort);

//       get_typlenbyval(orderbyType,
//               &indexstate->iss_OrderByTypLens[i],
//               &indexstate->iss_OrderByTypByVals[i]);
//       i++;
//     }

//     /* allocate arrays to hold the re-calculated distances */
//     indexstate->iss_OrderByValues = (Datum *)
//       palloc(numOrderByKeys * sizeof(Datum));
//     indexstate->iss_OrderByNulls = (bool *)
//       palloc(numOrderByKeys * sizeof(bool));

//     /* and initialize the reorder queue */
//     indexstate->iss_ReorderQueue = pairingheap_allocate(reorderqueue_cmp,
//                               indexstate);
//   }
  
//   /*
//    * If we have runtime keys, we need an ExprContext to evaluate them. The
//    * node's standard context won't do because we want to reset that context
//    * for every tuple.  So, build another context just like the other one...
//    * -tgl 7/11/00
//    */
//   if (indexstate->iss_NumRuntimeKeys != 0)
//   {
//     // ExprContext *stdecontext = p_node->base.ss.ps.ps_ExprContext;

//     // ExecAssignExprContext(estate, &p_node->base.ss.ps);
//     // indexstate->iss_RuntimeContext = p_node->base.ss.ps.ps_ExprContext;
//     // p_node->base.ss.ps.ps_ExprContext = stdecontext;
//   }
//   else
//   {
//     indexstate->iss_RuntimeContext = NULL;
//   }

//   /*
//    * Initialize scan descriptor.
//    */
//   indexstate->iss_ScanDesc = my_index_beginscan(currentRelation,
//                          indexstate->iss_RelationDesc,
//                          estate->es_snapshot,
//                          indexstate->iss_NumScanKeys,
//                        indexstate->iss_NumOrderByKeys);

//   /*
//    * If no run-time keys to calculate, go ahead and pass the scankeys to the
//    * index AM.
//    */
//   if (indexstate->iss_NumRuntimeKeys == 0)
//     my_index_rescan(indexstate->iss_ScanDesc,
//            indexstate->iss_ScanKeys, indexstate->iss_NumScanKeys,
//         indexstate->iss_OrderByKeys, indexstate->iss_NumOrderByKeys);
//   //TODO: index_rescan updates so (spgistScanOpaque) but for the scankeys only, I need to do the same
//   //        for the orderby keys

//   /*
//    * all done.
//    */
//    // elog(NOTICE , "Begin_BasicCustomScan ========= end");

//   /*
//    * Compute the Bounding Boxes for each page in the index
//    */
//   // start_Compute_BoundingBox(p_node->indexstate.iss_RelationDesc, p_node->indexid);
//   return;
// }

TupleTableSlot *  Exec_KInCircleScan (CustomScanState *node)
{
  // elog(NOTICE, "\nExec_KInCircleScan -------- start\n");
  
  TupleTableSlot * slot = ExecScan(&node->ss,
                                  (ExecScanAccessMtd) KInCircle_Next,
                                  (ExecScanRecheckMtd) KInCircle_Recheck);

  return slot;
}

static TupleTableSlot * KInCircle_Next(CustomScanState *node)
{
  TupleTableSlot *slot;
  // elog(NOTICE, "KInCircle_Next ---------- start");
  /*
   * Get the next tuple from the sub-query.
   */
  slot = ExecProcNode(linitial( node->custom_ps));
  /*
   * We just return the subplan's result slot, rather than expending extra
   * cycles for ExecCopySlot().  (Our own ScanTupleSlot is used only for
   * EvalPlanQual rechecks.)
   */
  return slot;
}
static bool KInCircle_Recheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true;
}

static TupleTableSlot * BasicCustomNext(CustomScanState *node)
{
  return NULL;
}
// {


//   KInCircleState * knnNode = (KInCircleState *) node;
//   IndexScanState * indexstate = &knnNode->indexstate;
  

//   ExprContext *econtext;
//   IndexScanDesc scandesc;
//   HeapTuple tuple;
//   TupleTableSlot *slot = NULL;
//   ReorderTuple *topmost = NULL;
//   bool    was_exact;
//   Datum    *lastfetched_vals;
//   bool     *lastfetched_nulls;
//   int     cmp;

//   // elog(NOTICE, "BasicCustomNext ---------- start");
  
//   scandesc = indexstate->iss_ScanDesc; //  ???
//   econtext = node->ss.ps.ps_ExprContext;
//   slot = node->ss.ss_ScanTupleSlot;

//   for (;;)
//   {
//     /*
//      * Check the reorder queue first.  If the topmost tuple in the queue
//      * has an ORDER BY value smaller than (or equal to) the value last
//      * returned by the index, we can return it now.
//      */
//     if (!pairingheap_is_empty(indexstate->iss_ReorderQueue))
//     {
//       // elog(NOTICE, "BasicCustomNext ---------- 1");
//       topmost = (ReorderTuple *) pairingheap_first(indexstate->iss_ReorderQueue);

//       if (indexstate->iss_ReachedEnd ||
//         cmp_orderbyvals(topmost->orderbyvals,
//                 topmost->orderbynulls,
//                 scandesc->xs_orderbyvals,
//                 scandesc->xs_orderbynulls,
//                 indexstate) <= 0)
//       {
//         // elog(NOTICE, "BasicCustomNext ---------- 2");
//         tuple = reorderqueue_pop(indexstate);
//         // elog(NOTICE, "BasicCustomNext ---------- 3");
//         /* Pass 'true', as the tuple in the queue is a palloc'd copy */
//         ExecStoreTuple(tuple, slot, InvalidBuffer, true);
//         // elog(NOTICE, "BasicCustomNext ---------- 4");
//         return slot;
//       }
//     }
//     else if (indexstate->iss_ReachedEnd)
//     {
//       /* Queue is empty, and no more tuples from index.  We're done. */
//       // elog(NOTICE, "BasicCustomNext ---------- 5");
//       return ExecClearTuple(slot);
//     }

//     /*
//      * Fetch next tuple from the index.
//      */
// next_indextuple:
//     // elog(NOTICE, "BasicCustomNext ---------- 6");
//     tuple = my_index_getnext(scandesc, ForwardScanDirection);
//     // elog(NOTICE, "BasicCustomNext ---------- 7");
//     if (!tuple)
//     {
//       /*
//        * No more tuples from the index.  But we still need to drain any
//        * remaining tuples from the queue before we're done.
//        */
//       indexstate->iss_ReachedEnd = true;
//       continue;
//     }

//     /*
//      * Store the scanned tuple in the scan tuple slot of the scan state.
//      * Note: we pass 'false' because tuples returned by amgetnext are
//      * pointers onto disk pages and must not be pfree()'d.
//      */
//     ExecStoreTuple(tuple, /* tuple to store */
//              slot,  /* slot to store in */
//              scandesc->xs_cbuf,   /* buffer containing tuple */
//              false);  /* don't pfree */

    
//      * If the index was lossy, we have to recheck the index quals and
//      * ORDER BY expressions using the fetched tuple.
     
//     if (scandesc->xs_recheck)
//     {
//       econtext->ecxt_scantuple = slot;
//       ResetExprContext(econtext);
//       if (!ExecQual(indexstate->indexqualorig, econtext, false))
//       {
//         /* Fails recheck, so drop it and loop back for another */
//         InstrCountFiltered2(indexstate, 1);
//         goto next_indextuple;
//       }
//     }

//     if (scandesc->xs_recheckorderby)
//     {
//       econtext->ecxt_scantuple = slot;
//       ResetExprContext(econtext);
//       EvalOrderByExpressions(indexstate, econtext);

//       /*
//        * Was the ORDER BY value returned by the index accurate?  The
//        * recheck flag means that the index can return inaccurate values,
//        * but then again, the value returned for any particular tuple
//        * could also be exactly correct.  Compare the value returned by
//        * the index with the recalculated value.  (If the value returned
//        * by the index happened to be exact right, we can often avoid
//        * pushing the tuple to the queue, just to pop it back out again.)
//        */
//       cmp = cmp_orderbyvals(indexstate->iss_OrderByValues,
//                   indexstate->iss_OrderByNulls,
//                   scandesc->xs_orderbyvals,
//                   scandesc->xs_orderbynulls,
//                   indexstate);
//       if (cmp < 0)
//         elog(ERROR, "index returned tuples in wrong order");
//       else if (cmp == 0)
//         was_exact = true;
//       else
//         was_exact = false;
//       lastfetched_vals = indexstate->iss_OrderByValues;
//       lastfetched_nulls = indexstate->iss_OrderByNulls;
//     }
//     else
//     {
//       was_exact = true;
//       lastfetched_vals = scandesc->xs_orderbyvals;
//       lastfetched_nulls = scandesc->xs_orderbynulls;
//     }

//     /*
//      * Can we return this tuple immediately, or does it need to be pushed
//      * to the reorder queue?  If the ORDER BY expression values returned
//      * by the index were inaccurate, we can't return it yet, because the
//      * next tuple from the index might need to come before this one. Also,
//      * we can't return it yet if there are any smaller tuples in the queue
//      * already.
//      */
//     if (!was_exact || (topmost && cmp_orderbyvals(lastfetched_vals,
//                             lastfetched_nulls,
//                             topmost->orderbyvals,
//                             topmost->orderbynulls,
//                             indexstate) > 0))
//     {
//       /* Put this tuple to the queue */
//       reorderqueue_push(indexstate, tuple, lastfetched_vals, lastfetched_nulls);
//       continue;
//     }
//     else
//     {
//       /* Can return this tuple immediately. */
//       return slot;
//     }
//   }

//   /*
//    * if we get here it means the index scan failed so we are at the end of
//    * the scan..
//    */
//   return ExecClearTuple(slot);
// }

/*
 * SubqueryRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool BasicCustomRecheck(CustomScanState *node, TupleTableSlot *slot)
{
  /* nothing to check */
  return true;
}

TupleTableSlot *  Exec_BasicCustomScan (CustomScanState *node)
{ 
  // elog(NOTICE, "Exec_BasicCustomScan -------- start");
  KInCircleState * knnNode = (KInCircleState *) node;
  // IndexScanState * indexstate = &knnNode->indexstate;
  return ExecScan(&(knnNode->base.ss),
          (ExecScanAccessMtd) BasicCustomNext,
          (ExecScanRecheckMtd) BasicCustomRecheck);
}



void    End_KInCircleScan (CustomScanState *node)
{
   // elog(NOTICE, "End_KInCircleScan -------- start");
  //End child plans
  ExecEndNode(linitial(node->custom_ps));
}

void    End_BasicCustomScan (CustomScanState *node)
{
  // elog(NOTICE, "End_BasicCustomScan -------- start");
  Relation  indexRelationDesc;
  IndexScanDesc indexScanDesc;
  // Relation  relation;

  KInCircleState * knnNode = (KInCircleState *) node;
  IndexScanState * indexstate = &knnNode->indexstate;
  
  /*
   * extract information from the node
   */
  indexRelationDesc = indexstate->iss_RelationDesc;
  indexScanDesc = indexstate->iss_ScanDesc;
  // relation = node->ss.ss_currentRelation;

  /*
   * Free the exprcontext(s) ... now dead code, see ExecFreeExprContext
   */
#ifdef NOT_USED
  ExecFreeExprContext(&node->ss.ps);
  if (indexstate->iss_RuntimeContext)
    FreeExprContext(indexstate->iss_RuntimeContext, true);
#endif

  /*
   * clear out tuple table slots
   */
  // ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
  // ExecClearTuple(node->ss.ss_ScanTupleSlot);

  /*
   * close the index relation (no-op if we didn't open it)
   */
  if (indexScanDesc)
    index_endscan(indexScanDesc);
  if (indexRelationDesc)
    index_close(indexRelationDesc, NoLock);

  /*
   * close the heap relation.
   */
  // ExecCloseScanRelation(relation);
}

void    ReScan_KInCircleScan (CustomScanState *node)
{}

void    ReScan_BasicCustomScan (CustomScanState *node)
{}


void my_ExecIndexBuildScanKeys(PlanState *planstate, Relation index,
             List *quals, bool isorderby,
             ScanKey *scanKeys, int *numScanKeys,
             IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys,
             IndexArrayKeyInfo **arrayKeys, int *numArrayKeys)
{
  ListCell   *qual_cell;
  ScanKey   scan_keys;
  IndexRuntimeKeyInfo *runtime_keys;
  IndexArrayKeyInfo *array_keys;
  int     n_scan_keys;
  int     n_runtime_keys;
  int     max_runtime_keys;
  int     n_array_keys;
  int     j;

  /* Allocate array for ScanKey structs: one per qual */
  n_scan_keys = list_length(quals);
  scan_keys = (ScanKey) palloc(n_scan_keys * sizeof(ScanKeyData));

  /*
   * runtime_keys array is dynamically resized as needed.  We handle it this
   * way so that the same runtime keys array can be shared between
   * indexquals and indexorderbys, which will be processed in separate calls
   * of this function.  Caller must be sure to pass in NULL/0 for first
   * call.
   */
  runtime_keys = *runtimeKeys;
  n_runtime_keys = max_runtime_keys = *numRuntimeKeys;

  /* Allocate array_keys as large as it could possibly need to be */
  array_keys = (IndexArrayKeyInfo *)
    palloc0(n_scan_keys * sizeof(IndexArrayKeyInfo));
  n_array_keys = 0;

  /*
   * for each opclause in the given qual, convert the opclause into a single
   * scan key
   */
  j = 0;
  foreach(qual_cell, quals)
  {
    Expr     *clause = (Expr *) lfirst(qual_cell);
    ScanKey   this_scan_key = &scan_keys[j++];
    Oid     opno;   /* operator's OID */
    RegProcedure opfuncid;  /* operator proc id used in scan */
    Oid     opfamily; /* opfamily of index column */
    int     op_strategy;  /* operator's strategy number */
    Oid     op_lefttype;  /* operator's declared input types */
    Oid     op_righttype;
    Expr     *leftop;   /* expr on lhs of operator */
    Expr     *rightop;  /* expr on rhs ... */
    AttrNumber  varattno; /* att number used in scan */

    if (IsA(clause, OpExpr))
    {
      /* indexkey op const or indexkey op expression */
      int     flags = 0;
      Datum   scanvalue;

      opno = ((OpExpr *) clause)->opno;
      opfuncid = ((OpExpr *) clause)->opfuncid;

      /*
       * leftop should be the index key Var, possibly relabeled
       */
      leftop = (Expr *) get_leftop(clause);

      if (leftop && IsA(leftop, RelabelType))
        leftop = ((RelabelType *) leftop)->arg;

      Assert(leftop != NULL);

      // if (!(IsA(leftop, Var) &&
      //     ((Var *) leftop)->varno == INDEX_VAR))
      //   elog(ERROR, "indexqual doesn't have key on left side");

      varattno = ((Var *) leftop)->varattno;
      // if (varattno < 1 || varattno > index->rd_index->indnatts)
      //   elog(ERROR, "bogus index qualification");

      /*
       * We have to look up the operator's strategy number.  This
       * provides a cross-check that the operator does match the index.
       */
      //opfamily = index->rd_opfamily[varattno - 1];

      // get_op_opfamily_properties(opno, opfamily, isorderby,
      //                &op_strategy,
      //                &op_lefttype,
      //                &op_righttype);
        opfamily = 4015;
        op_strategy = 6; // this is for RTSameStrategyNumber TODO: need to implement consisten function that handles NNsearchStrategy
      op_lefttype = 600;
      op_righttype = 600;

      if (isorderby)
        flags |= SK_ORDER_BY;

      /*
       * rightop is the constant or variable comparison value
       */
      rightop = (Expr *) get_rightop(clause);

      if (rightop && IsA(rightop, RelabelType))
        rightop = ((RelabelType *) rightop)->arg;

      Assert(rightop != NULL);

      if (IsA(rightop, Const))
      {
        /* OK, simple constant comparison value */
        scanvalue = ((Const *) rightop)->constvalue;
        if (((Const *) rightop)->constisnull)
          flags |= SK_ISNULL;
      }
      else
      {
        /* Need to treat this one as a runtime key */
        if (n_runtime_keys >= max_runtime_keys)
        {
          if (max_runtime_keys == 0)
          {
            max_runtime_keys = 8;
            runtime_keys = (IndexRuntimeKeyInfo *)
              palloc(max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
          }
          else
          {
            max_runtime_keys *= 2;
            runtime_keys = (IndexRuntimeKeyInfo *)
              repalloc(runtime_keys, max_runtime_keys * sizeof(IndexRuntimeKeyInfo));
          }
        }
        runtime_keys[n_runtime_keys].scan_key = this_scan_key;
        runtime_keys[n_runtime_keys].key_expr =
          ExecInitExpr(rightop, planstate);
        runtime_keys[n_runtime_keys].key_toastable =
          TypeIsToastable(op_righttype);
        n_runtime_keys++;
        scanvalue = (Datum) 0;
      }

      /*
       * initialize the scan key's fields appropriately
       */
      ScanKeyEntryInitialize(this_scan_key,
                   flags,
                   varattno,  /* attribute number to scan */
                   op_strategy, /* op's strategy */
                   op_righttype,    /* strategy subtype */
                   ((OpExpr *) clause)->inputcollid,  /* collation */
                   opfuncid,  /* reg proc to use */
                   scanvalue);  /* constant */
      

    }
    
  }

  Assert(n_runtime_keys <= max_runtime_keys);

  /* Get rid of any unused arrays */
  if (n_array_keys == 0)
  {
    pfree(array_keys);
    array_keys = NULL;
  }

  /*
   * Return info to our caller.
   */
  *scanKeys = scan_keys;
  *numScanKeys = n_scan_keys;
  *runtimeKeys = runtime_keys;
  *numRuntimeKeys = n_runtime_keys;
  if (arrayKeys)
  {
    *arrayKeys = array_keys;
    *numArrayKeys = n_array_keys;
  }
  else if (n_array_keys != 0)
    elog(ERROR, "ScalarArrayOpExpr index qual found where not allowed");
}

// static int reorderqueue_cmp(const pairingheap_node *a,
//          const pairingheap_node *b, void *arg)
// {
//   ReorderTuple *rta = (ReorderTuple *) a;
//   ReorderTuple *rtb = (ReorderTuple *) b;
//   IndexScanState *node = (IndexScanState *) arg;

//   return -cmp_orderbyvals(rta->orderbyvals, rta->orderbynulls,
//               rtb->orderbyvals, rtb->orderbynulls,
//               node);
// }

/*
 * Compare ORDER BY expression values.
 */

// static int
// cmp_orderbyvals(const Datum *adist, const bool *anulls,
//         const Datum *bdist, const bool *bnulls,
//         IndexScanState *node)
// {
//   int     i;
//   int     result;

//   for (i = 0; i < node->iss_NumOrderByKeys; i++)
//   {
//     SortSupport ssup = &node->iss_SortSupport[i];

//     /*
//      * Handle nulls.  We only need to support NULLS LAST ordering, because
//      * match_pathkeys_to_index() doesn't consider indexorderby
//      * implementation otherwise.
//      */
//     if (anulls[i] && !bnulls[i])
//       return 1;
//     else if (!anulls[i] && bnulls[i])
//       return -1;
//     else if (anulls[i] && bnulls[i])
//       return 0;

//     result = ssup->comparator(adist[i], bdist[i], ssup);
//     if (result != 0)
//       return result;
//   }

//   return 0;
// }

/*
 * Helper function to pop the next tuple from the reorder queue.
 */
// static HeapTuple
// reorderqueue_pop(IndexScanState *node)
// {
//   HeapTuple result;
//   ReorderTuple *topmost;
//   int     i;

//   topmost = (ReorderTuple *) pairingheap_remove_first(node->iss_ReorderQueue);

//   result = topmost->htup;
//   for (i = 0; i < node->iss_NumOrderByKeys; i++)
//   {
//     if (!node->iss_OrderByTypByVals[i] && !topmost->orderbynulls[i])
//       pfree(DatumGetPointer(topmost->orderbyvals[i]));
//   }
//   pfree(topmost->orderbyvals);
//   pfree(topmost->orderbynulls);
//   pfree(topmost);

//   return result;
// }
/*
 * Helper function to push a tuple to the reorder queue.
 */
// static void
// reorderqueue_push(IndexScanState *node, HeapTuple tuple,
//           Datum *orderbyvals, bool *orderbynulls)
// {
//   IndexScanDesc scandesc = node->iss_ScanDesc;
//   EState     *estate = node->ss.ps.state;
//   MemoryContext oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
//   ReorderTuple *rt;
//   int     i;

//   rt = (ReorderTuple *) palloc(sizeof(ReorderTuple));
//   rt->htup = heap_copytuple(tuple);
//   rt->orderbyvals =
//     (Datum *) palloc(sizeof(Datum) * scandesc->numberOfOrderBys);
//   rt->orderbynulls =
//     (bool *) palloc(sizeof(bool) * scandesc->numberOfOrderBys);
//   for (i = 0; i < node->iss_NumOrderByKeys; i++)
//   {
//     if (!orderbynulls[i])
//       rt->orderbyvals[i] = datumCopy(orderbyvals[i],
//                        node->iss_OrderByTypByVals[i],
//                        node->iss_OrderByTypLens[i]);
//     else
//       rt->orderbyvals[i] = (Datum) 0;
//     rt->orderbynulls[i] = orderbynulls[i];
//   }
//   pairingheap_add(node->iss_ReorderQueue, &rt->ph_node);

//   MemoryContextSwitchTo(oldContext);
// }
/*
 * Calculate the expressions in the ORDER BY clause, based on the heap tuple.
 */
// static void
// EvalOrderByExpressions(IndexScanState *node, ExprContext *econtext)
// {
//   int     i;
//   ListCell   *l;
//   MemoryContext oldContext;

//   oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

//   i = 0;
//   foreach(l, node->indexorderbyorig)
//   {
//     ExprState  *orderby = (ExprState *) lfirst(l);

//     node->iss_OrderByValues[i] = ExecEvalExpr(orderby,
//                           econtext,
//                           &node->iss_OrderByNulls[i],
//                           NULL);
//     i++;
//   }

//   MemoryContextSwitchTo(oldContext);
// }

// IndexScanDesc
// my_index_beginscan(Relation heapRelation,
//         Relation indexRelation,
//         Snapshot snapshot,
//         int nkeys, int norderbys)
// {
//   IndexScanDesc scan;

//   scan = my_index_beginscan_internal(indexRelation, nkeys, norderbys, snapshot);

//   /*
//    * Save additional parameters into the scandesc.  Everything else was set
//    * up by RelationGetIndexScan.
//    */
//   scan->heapRelation = heapRelation;
//   scan->xs_snapshot = snapshot;

//   return scan;
// }

/*
 * index_beginscan_internal --- common code for index_beginscan variants
 */
// static IndexScanDesc
// my_index_beginscan_internal(Relation indexRelation,
//              int nkeys, int norderbys, Snapshot snapshot)
// {
//   //RELATION_CHECKS;
//   //CHECK_REL_PROCEDURE(ambeginscan);

//   if (!(indexRelation->rd_amroutine->ampredlocks))
//     PredicateLockRelation(indexRelation, snapshot);

  
//    * We hold a reference count to the relcache entry throughout the scan.
   
//   RelationIncrementReferenceCount(indexRelation);

//   /*
//    * Tell the AM to open a scan.
//    */
//   // return indexRelation->rd_amroutine->ambeginscan(indexRelation, nkeys,
//   //                         norderbys);
//   //TODO: call the modified beginscan for the spgist scan 
//   return my_spgbeginscan(indexRelation, nkeys,
//                           norderbys);
// }




// IndexScanDesc
// my_spgbeginscan(Relation rel, int keysz, int orderbysz)
// {
//   IndexScanDesc scan;
//   my_SpGistScanOpaque so;
//   MemoryContext oldCxt;


//   scan = RelationGetIndexScan(rel, keysz, orderbysz);
//   so = (my_SpGistScanOpaque) palloc0(sizeof(my_SpGistScanOpaqueData));

//   so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
//                     "SP-GiST search temporary context",
//                     ALLOCSET_DEFAULT_SIZES);
//   so->scanCxt = AllocSetContextCreate(CurrentMemoryContext,
//                   "SP-GiST scan context",
//                   ALLOCSET_DEFAULT_SIZES);

//   oldCxt = MemoryContextSwitchTo(so->scanCxt);

  
//   if (keysz > 0)
//     so->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * keysz);
//   else
//     so->keyData = NULL;
  
//   //initialize the state
//   initSpGistState(&so->state, scan->indexRelation);

//   /* Set up indexTupDesc and xs_itupdesc in case it's an index-only scan */
//   so->indexTupDesc = scan->xs_itupdesc = RelationGetDescr(rel);


//   /* -----------------------------------
//    * --------- ORDER By support  ------- 
//    * ----------------------------------- */
//   so->queue = NULL;
//   so->queueCxt = so->scanCxt;  /* see gistrescan */

//   /* workspaces with size dependent on numberOfOrderBys: */
//   so->distances = palloc(sizeof(double) * scan->numberOfOrderBys);
//   so->qual_ok = true;     /* in case there are zero keys */
//   if (scan->numberOfOrderBys > 0)
//   {
//     scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
//     scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
//     memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
//   }

//   so->killedItems = NULL;   /* until needed */
//   so->numKilled = 0;
//   so->curBlkno = InvalidBlockNumber;
//   //so->curPageLSN = InvalidXLogRecPtr;


//   scan->opaque = so;
//   MemoryContextSwitchTo(oldCxt);
//   return scan;
// }

IndexScanDesc
myspgbeginscan(Relation rel, int keysz, int orderbysz)
{
  IndexScanDesc scan;
  mySpGistScanOpaque so;
  
  scan = RelationGetIndexScan(rel, keysz, orderbysz);

  so = (mySpGistScanOpaque) palloc0(sizeof(mySpGistScanOpaqueData));
  
  if (keysz > 0)
    so->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * keysz);
  else
    so->keyData = NULL;
  
  initSpGistState(&so->state, scan->indexRelation);
  
  so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
                    "SP-GiST search temporary context",
                    ALLOCSET_DEFAULT_SIZES);

  /* Set up indexTupDesc and xs_itupdesc in case it's an index-only scan */
  so->indexTupDesc = scan->xs_itupdesc = RelationGetDescr(rel);

  /* -----------------------------------
   * --------- ORDER By support  ------- 
   * ----------------------------------- */
  MemoryContext oldCxt;
  so->queue = NULL;
  so->scanCxt = AllocSetContextCreate(CurrentMemoryContext,
                  "SP-GiST scan context",
                  ALLOCSET_DEFAULT_SIZES);

  oldCxt = MemoryContextSwitchTo(so->scanCxt);
  so->queueCxt = so->scanCxt;  /* see gistrescan */

  /* workspaces with size dependent on numberOfOrderBys: */
  so->distances = palloc(sizeof(double) * scan->numberOfOrderBys);
  so->qual_ok = true;     /* in case there are zero keys */
  if (scan->numberOfOrderBys > 0)
  {
    scan->xs_orderbyvals = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
    scan->xs_orderbynulls = palloc(sizeof(bool) * scan->numberOfOrderBys);
    memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
  }

  so->killedItems = NULL;   /* until needed */
  so->numKilled = 0;
  so->curBlkno = InvalidBlockNumber;
  //so->curPageLSN = InvalidXLogRecPtr;


  scan->opaque = so;
  MemoryContextSwitchTo(oldCxt);


  // DEBUG
  // Compute_BoundingBoxes(scan->indexRelation  ); 
  // DEBUG
  return scan;
}


/* ----------------
 *    index_rescan  - (re)start a scan of an index
 *
 * During a restart, the caller may specify a new set of scankeys and/or
 * orderbykeys; but the number of keys cannot differ from what index_beginscan
 * was told.  (Later we might relax that to "must not exceed", but currently
 * the index AMs tend to assume that scan->numberOfKeys is what to believe.)
 * To restart the scan without changing keys, pass NULL for the key arrays.
 * (Of course, keys *must* be passed on the first call, unless
 * scan->numberOfKeys is zero.)
 * ----------------
 */

// void
// my_index_rescan(IndexScanDesc scan,
//        ScanKey keys, int nkeys,
//        ScanKey orderbys, int norderbys)
// {
//   // SCAN_CHECKS;
//   // CHECK_SCAN_PROCEDURE(amrescan);

//   Assert(nkeys == scan->numberOfKeys);
//   Assert(norderbys == scan->numberOfOrderBys);

//   /* Release any held pin on a heap page */
//   if (BufferIsValid(scan->xs_cbuf))
//   {
//     ReleaseBuffer(scan->xs_cbuf);
//     scan->xs_cbuf = InvalidBuffer;
//   }

//   scan->xs_continue_hot = false;

//   scan->kill_prior_tuple = false;   /* for safety */

//   // scan->indexRelation->rd_amroutine->amrescan(scan, keys, nkeys,
//   //                       orderbys, norderbys);
//   // ORDER BY support for spGist 
//   my_spgrescan(scan, keys, nkeys,
//                         orderbys, norderbys);
// }


// void
// my_spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
//       ScanKey orderbys, int norderbys)
// {
//   my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;

//   /* copy scankeys into local storage */
//   if (scankey && scan->numberOfKeys > 0)
//   {
//     memmove(scan->keyData, scankey,
//         scan->numberOfKeys * sizeof(ScanKeyData));
//   }

//   //TODO ^_^ : need handling
//   /* preprocess scankeys, set up the representation in *so */
//   spgPrepareScanKeys(scan);

//   /* set up starting stack entries */
//   // resetSpGistScanOpaque(so);

//   /* -------------------------------------
//    * ------ Order By Support -------------
//    * ------------------------------------- */

//   bool    first_time;
//   int     i;
//   MemoryContext oldCxt;

//   /*
//    * The first time through, we create the search queue in the scanCxt.
//    * Subsequent times through, we create the queue in a separate queueCxt,
//    * which is created on the second call and reset on later calls.  Thus, in
//    * the common case where a scan is only rescan'd once, we just put the
//    * queue in scanCxt and don't pay the overhead of making a second memory
//    * context.  If we do rescan more than once, the first queue is just left
//    * for dead until end of scan; this small wastage seems worth the savings
//    * in the common case.
//    */
//   if (so->queue == NULL)
//   {
//     /* first time through */
//     Assert(so->queueCxt == so->scanCxt);
//     first_time = true;
//   }
//   else if (so->queueCxt == so->scanCxt)
//   {
//     /* second time through */
//     so->queueCxt = AllocSetContextCreate(so->scanCxt,
//                        "spGiST queue context",
//                        ALLOCSET_DEFAULT_SIZES);
//     first_time = false;
//   }
//   else
//   {
//     /* third or later time through */
//     MemoryContextReset(so->queueCxt);
//     first_time = false;
//   }

//   /*
//    * If we're doing an index-only scan, on the first call, also initialize a
//    * tuple descriptor to represent the returned index tuples and create a
//    * memory context to hold them during the scan.
//    */
//   // if (scan->xs_want_itup && !scan->xs_itupdesc)
//   // {
//   //   int     natts;
//   //   int     attno;

//   //   /*
//   //    * The storage type of the index can be different from the original
//   //    * datatype being indexed, so we cannot just grab the index's tuple
//   //    * descriptor. Instead, construct a descriptor with the original data
//   //    * types.
//   //    */
//   //   natts = RelationGetNumberOfAttributes(scan->indexRelation);
//   //   so->giststate->fetchTupdesc = CreateTemplateTupleDesc(natts, false);
//   //   for (attno = 1; attno <= natts; attno++)
//   //   {
//   //     TupleDescInitEntry(so->giststate->fetchTupdesc, attno, NULL,
//   //                scan->indexRelation->rd_opcintype[attno - 1],
//   //                -1, 0);
//   //   }
//   //   scan->xs_itupdesc = so->giststate->fetchTupdesc;

//   //   so->pageDataCxt = AllocSetContextCreate(so->giststate->scanCxt,
//   //                       "GiST page data context",
//   //                       ALLOCSET_DEFAULT_SIZES);
//   // }

//   /* create new, empty pairing heap for search queue */
//   oldCxt = MemoryContextSwitchTo(so->queueCxt);
//   so->queue = pairingheap_allocate(pairingheap_SpGISTSearchItem_cmp, scan);
//   MemoryContextSwitchTo(oldCxt);

//   so->firstCall = true;

//   /* Update order-by key, if a new one is given */
//   if (orderbys && scan->numberOfOrderBys > 0)
//   {
//     void    **fn_extras = NULL;

//     /* As above, preserve fn_extra if not first time through */
//     if (!first_time)
//     {
//       fn_extras = (void **) palloc(scan->numberOfOrderBys * sizeof(void *));
//       for (i = 0; i < scan->numberOfOrderBys; i++)
//         fn_extras[i] = scan->orderByData[i].sk_func.fn_extra;
//     }

//     memmove(scan->orderByData, orderbys,
//         scan->numberOfOrderBys * sizeof(ScanKeyData));

//     so->orderByTypes = (Oid *) palloc(scan->numberOfOrderBys * sizeof(Oid));

//     /*
//      * Modify the order-by key so that the Distance method is called for
//      * all comparisons. The original operator is passed to the Distance
//      * function in the form of its strategy number, which is available
//      * from the sk_strategy field, and its subtype from the sk_subtype
//      * field.
//      */
//     for (i = 0; i < scan->numberOfOrderBys; i++)
//     {
//       ScanKey   skey = scan->orderByData + i;
//       //TODO: finfo: n
//       FmgrInfo   *finfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));//&(so->giststate->distanceFn[skey->sk_attno - 1]);
//       //finfo->
//       /* Check we actually have a distance function ... */
//       // if (!OidIsValid(finfo->fn_oid))
//       //   elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
//       //      GIST_DISTANCE_PROC, skey->sk_attno,
//       //      RelationGetRelationName(scan->indexRelation));

//       /*
//        * Look up the datatype returned by the original ordering
//        * operator. GiST always uses a float8 for the distance function,
//        * but the ordering operator could be anything else.
//        *
//        * XXX: The distance function is only allowed to be lossy if the
//        * ordering operator's result type is float4 or float8.  Otherwise
//        * we don't know how to return the distance to the executor.  But
//        * we cannot check that here, as we won't know if the distance
//        * function is lossy until it returns *recheck = true for the
//        * first time.
//        */
//       so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);

//       /*
//        * Copy distance support function to ScanKey structure instead of
//        * function implementing ordering operator.
//        */
//         // TODO: need to make sure that whenever we use this sk_func I will call distance function
//       fmgr_info_copy(&(skey->sk_func), finfo, so->scanCxt);

//       /* Restore prior fn_extra pointers, if not first time */
//       if (!first_time)
//         skey->sk_func.fn_extra = fn_extras[i];
//     }

//     if (!first_time)
//       pfree(fn_extras);
//   }

//   /* any previous xs_itup will have been pfree'd in context resets above */
//   scan->xs_itup = NULL;
// }

void
myspgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
      ScanKey orderbys, int norderbys)
{

  spgrescan( scan, scankey, nscankeys,
       orderbys, norderbys);
  // mySpGistScanOpaque so = (mySpGistScanOpaque) scan->opaque;

  // /* copy scankeys into local storage */
  // if (scankey && scan->numberOfKeys > 0)
  // {
  //   memmove(scan->keyData, scankey,
  //       scan->numberOfKeys * sizeof(ScanKeyData));
  // }

  // //TODO ^_^ : need handling
  // /* preprocess scankeys, set up the representation in *so */
  // spgPrepareScanKeys(scan);

  /* set up starting stack entries */
  // resetSpGistScanOpaque(so);

  /* -------------------------------------
   * ------ Order By Support -------------
   * ------------------------------------- */

  mySpGistScanOpaque so = (mySpGistScanOpaque) scan->opaque;
  bool    first_time;
  int     i;
  MemoryContext oldCxt;
  
  /*
   * The first time through, we create the search queue in the scanCxt.
   * Subsequent times through, we create the queue in a separate queueCxt,
   * which is created on the second call and reset on later calls.  Thus, in
   * the common case where a scan is only rescan'd once, we just put the
   * queue in scanCxt and don't pay the overhead of making a second memory
   * context.  If we do rescan more than once, the first queue is just left
   * for dead until end of scan; this small wastage seems worth the savings
   * in the common case.
   */
  if (so->queue == NULL)
  {
    /* first time through */
    Assert(so->queueCxt == so->scanCxt);
    first_time = true;
  }
  else if (so->queueCxt == so->scanCxt)
  {
    /* second time through */
    so->queueCxt = AllocSetContextCreate(so->scanCxt,
                       "spGiST queue context",
                       ALLOCSET_DEFAULT_SIZES);
    first_time = false;
  }
  else
  {
    /* third or later time through */
    MemoryContextReset(so->queueCxt);
    first_time = false;
  }


  /* create new, empty pairing heap for search queue */
  oldCxt = MemoryContextSwitchTo(so->queueCxt);
  so->queue = pairingheap_allocate(pairingheap_SpGISTSearchItem_cmp, scan);
  MemoryContextSwitchTo(oldCxt);

  so->firstCall = true;

  /* Update order-by key, if a new one is given */
  if (orderbys && scan->numberOfOrderBys > 0)
  {
    void    **fn_extras = NULL;

    /* As above, preserve fn_extra if not first time through */
    if (!first_time)
    {
      fn_extras = (void **) palloc(scan->numberOfOrderBys * sizeof(void *));
      for (i = 0; i < scan->numberOfOrderBys; i++)
        fn_extras[i] = scan->orderByData[i].sk_func.fn_extra;
    }

    memmove(scan->orderByData, orderbys,
        scan->numberOfOrderBys * sizeof(ScanKeyData));

    so->orderByTypes = (Oid *) palloc(scan->numberOfOrderBys * sizeof(Oid));

    /*
     * Modify the order-by key so that the Distance method is called for
     * all comparisons. The original operator is passed to the Distance
     * function in the form of its strategy number, which is available
     * from the sk_strategy field, and its subtype from the sk_subtype
     * field.
     */
    for (i = 0; i < scan->numberOfOrderBys; i++)
    {
      ScanKey   skey = scan->orderByData + i;
      //TODO: finfo: n
      // FmgrInfo   *finfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));//&(so->giststate->distanceFn[skey->sk_attno - 1]);
      FmgrInfo   *finfo = index_getprocinfo(scan->indexRelation, 1, SPGIST_DISTANCE_POINT_PROC); // ?? 1
      /* Check we actually have a distance function ... */
      if (!OidIsValid(finfo->fn_oid))
        elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
           SPGIST_DISTANCE_POINT_PROC, skey->sk_attno,
           RelationGetRelationName(scan->indexRelation));

      
      /*
       * Look up the datatype returned by the original ordering
       * operator. GiST always uses a float8 for the distance function,
       * but the ordering operator could be anything else.
       *
       * XXX: The distance function is only allowed to be lossy if the
       * ordering operator's result type is float4 or float8.  Otherwise
       * we don't know how to return the distance to the executor.  But
       * we cannot check that here, as we won't know if the distance
       * function is lossy until it returns *recheck = true for the
       * first time.
       */
      so->orderByTypes[i] = get_func_rettype(skey->sk_func.fn_oid);

      /*
       * Copy distance support function to ScanKey structure instead of
       * function implementing ordering operator.
       */
        // TODO: need to make sure that whenever we use this sk_func I will call distance function
      fmgr_info_copy(&(skey->sk_func), finfo, so->scanCxt);

      /* Restore prior fn_extra pointers, if not first time */
      if (!first_time)
        skey->sk_func.fn_extra = fn_extras[i];
    }

    if (!first_time)
      pfree(fn_extras);
  }

  /* any previous xs_itup will have been pfree'd in context resets above */
  scan->xs_itup = NULL;
}

/*
 * Prepare scan keys in SpGistScanOpaque from caller-given scan keys
 *
 * Sets searchNulls, searchNonNulls, numberOfKeys, keyData fields of *so.
 *
 * The point here is to eliminate null-related considerations from what the
 * opclass consistent functions need to deal with.  We assume all SPGiST-
 * indexable operators are strict, so any null RHS value makes the scan
 * condition unsatisfiable.  We also pull out any IS NULL/IS NOT NULL
 * conditions; their effect is reflected into searchNulls/searchNonNulls.
 */
// static void
// spgPrepareScanKeys(IndexScanDesc scan)
// {
//   SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
//   bool    qual_ok;
//   bool    haveIsNull;
//   bool    haveNotNull;
//   int     nkeys;
//   int     i;

//   if (scan->numberOfKeys <= 0)
//   {
//     /* If no quals, whole-index scan is required */
//     so->searchNulls = true;
//     so->searchNonNulls = true;
//     so->numberOfKeys = 0;
//     return;
//   }

//   /* Examine the given quals */
//   qual_ok = true;
//   haveIsNull = haveNotNull = false;
//   nkeys = 0;
//   for (i = 0; i < scan->numberOfKeys; i++)
//   {
//     ScanKey   skey = &scan->keyData[i];

//     if (skey->sk_flags & SK_SEARCHNULL)
//       haveIsNull = true;
//     else if (skey->sk_flags & SK_SEARCHNOTNULL)
//       haveNotNull = true;
//     else if (skey->sk_flags & SK_ISNULL)
//     {
//       /* ordinary qual with null argument - unsatisfiable */
//       qual_ok = false;
//       break;
//     }
//     else
//     {
//       /* ordinary qual, propagate into so->keyData */
//       so->keyData[nkeys++] = *skey;
//       /* this effectively creates a not-null requirement */
//       haveNotNull = true;
//     }
//   }

//   /* IS NULL in combination with something else is unsatisfiable */
//   if (haveIsNull && haveNotNull)
//     qual_ok = false;

//   /* Emit results */
//   if (qual_ok)
//   {
//     so->searchNulls = haveIsNull;
//     so->searchNonNulls = haveNotNull;
//     so->numberOfKeys = nkeys;
//   }
//   else
//   {
//     so->searchNulls = false;
//     so->searchNonNulls = false;
//     so->numberOfKeys = 0;
//   }
// }



/*
 * Pairing heap comparison function for the GISTSearchItem queue
 */
static int
pairingheap_SpGISTSearchItem_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
  const SPGISTSearchItem *sa = (const SPGISTSearchItem *) a;
  const SPGISTSearchItem *sb = (const SPGISTSearchItem *) b;
  IndexScanDesc scan = (IndexScanDesc) arg;
  int     i;

  /* Order according to distance comparison */
  for (i = 0; i < scan->numberOfOrderBys; i++)
  {
    if (sa->distances[i] != sb->distances[i])
      return (sa->distances[i] < sb->distances[i]) ? 1 : -1;
  }

  /* Heap items go before inner pages, to ensure a depth-first search */
  if (SPGISTSearchItemIsHeap(*sa) && !SPGISTSearchItemIsHeap(*sb))
    return 1;
  if (!SPGISTSearchItemIsHeap(*sa) && SPGISTSearchItemIsHeap(*sb))
    return -1;

  return 0;
}

#define point_point_distance(p1,p2) \
  DatumGetFloat8(DirectFunctionCall2(point_distance, \
                     PointPGetDatum(p1), PointPGetDatum(p2)))

static double
computeDistance(bool isLeaf, BOX *box, Point *point)
{
  // elog(NOTICE, "computeDistance ----------- start");
  double    result = 0.0;
   if (isLeaf)
  {
    // elog(NOTICE, "computeDistance ----------- 1");
    /* simple point to point distance */
    result = point_point_distance(point, &box->low);
  }
  else if (point->x <= box->high.x && point->x >= box->low.x &&
       point->y <= box->high.y && point->y >= box->low.y)
  {
    // elog(NOTICE, "computeDistance ----------- 2");
    /* point inside the box */
    result = 0.0;
  }
  else if (point->x <= box->high.x && point->x >= box->low.x)
  {
    // elog(NOTICE, "computeDistance ----------- 3");
    /* point is over or below box */
    Assert(box->low.y <= box->high.y);
    if (point->y > box->high.y)
      result = point->y - box->high.y;
    else if (point->y < box->low.y)
      result = box->low.y - point->y;
    else
      elog(ERROR, "inconsistent point values");
  }
  else if (point->y <= box->high.y && point->y >= box->low.y)
  {
    // elog(NOTICE, "computeDistance ----------- 4");
    /* point is to left or right of box */
    Assert(box->low.x <= box->high.x);
    if (point->x > box->high.x)
      result = point->x - box->high.x;
    else if (point->x < box->low.x)
      result = box->low.x - point->x;
    else
      elog(ERROR, "inconsistent point values");
  }
  else
  {
    // elog(NOTICE, "computeDistance ----------- 5");
    /* closest point will be a vertex */
    Point   p;
    double    subresult;

    result = point_point_distance(point, &box->low);

    subresult = point_point_distance(point, &box->high);
    if (result > subresult)
      result = subresult;

    p.x = box->low.x;
    p.y = box->high.y;
    subresult = point_point_distance(point, &p);
    if (result > subresult)
      result = subresult;

    p.x = box->high.x;
    p.y = box->low.y;
    subresult = point_point_distance(point, &p);
    if (result > subresult)
      result = subresult;
  }
  return result;
}



/* ----------------
 *    index_getnext - get the next heap tuple from a scan
 *
 * The result is the next heap tuple satisfying the scan keys and the
 * snapshot, or NULL if no more matching tuples exist.
 *
 * On success, the buffer containing the heap tup is pinned (the pin will be
 * dropped in a future index_getnext_tid, index_fetch_heap or index_endscan
 * call).
 *
 * Note: caller must check scan->xs_recheck, and perform rechecking of the
 * scan keys if required.  We do not do that here because we don't have
 * enough information to do it efficiently in the general case.
 * ----------------
 */
// HeapTuple
// my_index_getnext(IndexScanDesc scan, ScanDirection direction)
// {
//   // elog(NOTICE, "my_index_getnext ---------- start");
//   HeapTuple heapTuple;
//   ItemPointer tid;

//   for (;;)
//   {
//     if (scan->xs_continue_hot)
//     {
//       // elog(NOTICE, "my_index_getnext ---------- 1");
//       /*
//        * We are resuming scan of a HOT chain after having returned an
//        * earlier member.  Must still hold pin on current heap page.
//        */
//       Assert(BufferIsValid(scan->xs_cbuf));
//       // elog(NOTICE, "my_index_getnext ---------- 2");
//       Assert(ItemPointerGetBlockNumber(&scan->xs_ctup.t_self) ==
//            BufferGetBlockNumber(scan->xs_cbuf));
//     }
//     else
//     {
//       // elog(NOTICE, "my_index_getnext ---------- 3");
//       /* Time to fetch the next TID from the index */
//       tid = my_index_getnext_tid(scan, direction);

//       /* If we're out of index entries, we're done */
//       if (tid == NULL)
//         break;
//     }

//     /*
//      * Fetch the next (or only) visible heap tuple for this index entry.
//      * If we don't find anything, loop around and grab the next TID from
//      * the index.
//      */
//     // elog(NOTICE, "my_index_getnext ---------- 4");
//     heapTuple = index_fetch_heap(scan);
//     // elog(NOTICE, "my_index_getnext ---------- 5");
//     if (heapTuple != NULL)
//       return heapTuple;
//   }
//   // elog(NOTICE, "my_index_getnext ---------- 6");
//   return NULL;        /* failure exit */
// }

/* ----------------
 * index_getnext_tid - get the next TID from a scan
 *
 * The result is the next TID satisfying the scan keys,
 * or NULL if no more matching tuples exist.
 * ----------------
 */
// ItemPointer
// my_index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
// {
//   bool    found;

//   // SCAN_CHECKS;
//   // CHECK_SCAN_PROCEDURE(amgettuple);

//   // Assert(TransactionIdIsValid(RecentGlobalXmin));

//   /*
//    * The AM's amgettuple proc finds the next index entry matching the scan
//    * keys, and puts the TID into scan->xs_ctup.t_self.  It should also set
//    * scan->xs_recheck and possibly scan->xs_itup, though we pay no attention
//    * to those fields here.
//    */
//   //found = scan->indexRelation->rd_amroutine->amgettuple(scan, direction);
//   found = my_spggettuple(scan, direction);
  
//   /* Reset kill flag immediately for safety */
//   scan->kill_prior_tuple = false;

//   /* If we're out of index entries, we're done */
//   if (!found)
//   {
//     /* ... but first, release any held pin on a heap page */
//     if (BufferIsValid(scan->xs_cbuf))
//     {
//       ReleaseBuffer(scan->xs_cbuf);
//       scan->xs_cbuf = InvalidBuffer;
//     }
//     return NULL;
//   }

//   pgstat_count_index_tuples(scan->indexRelation, 1);

//   /* Return the TID of the tuple we found. */
//   return &scan->xs_ctup.t_self;
// }


// bool
// my_spggettuple(IndexScanDesc scan, ScanDirection dir)
// {
//   // elog(NOTICE, "my_spggettuple ----------- start");

//   my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
//   bool LeafReached = false;
//   if (dir != ForwardScanDirection)
//     elog(ERROR, "SpGiST only supports forward scan direction");

//   if (!so->qual_ok)
//     return false;

//   if (so->firstCall)
//   {
//     // elog(NOTICE, "my_spggettuple ----------- 1");
//     /* Begin the scan by processing the root page */
//     SPGISTSearchItem fakeItem;
//     // GISTSearchItem fakeItem;

//     pgstat_count_index_scan(scan->indexRelation);

//     so->firstCall = false;
//     so->curPageData = so->nPageData = 0;
//     scan->xs_itup = NULL;
    
//     fakeItem.blkno = SPGIST_ROOT_BLKNO;
//     BlockIdSet(&(fakeItem.ptr.ip_blkid), SPGIST_ROOT_BLKNO);
    
//     fakeItem.ptr.ip_posid = FirstOffsetNumber;
//     fakeItem.level = 0;
//     fakeItem.P_min.x = 0.0;
//     fakeItem.P_min.y = 0.0;
    
//     // elog(NOTICE, "my_spggettuple ----------- 2");
//     // spgistScanPage2(scan, &fakeItem, &LeafReached);
//     spgistScanPage3(scan, &fakeItem, &LeafReached);
//     // elog(NOTICE, "my_spggettuple ----------- 3");
//   }

//   //if (scan->numberOfOrderBys > 0)
//   {
//     // elog(NOTICE, "my_spggettuple ----------- 4");
//     /* Must fetch tuples in strict distance order */
//     return my_getNextNearest(scan);
//   }
// }

bool
myspggettuple(IndexScanDesc scan, ScanDirection dir)
{
  // elog(NOTICE, "my_spggettuple ----------- start");

  if (scan->numberOfOrderBys == 0)
  {
    return spggettuple(scan, dir);
  }
  else
  {
    mySpGistScanOpaque so = (mySpGistScanOpaque) scan->opaque;
    bool LeafReached = false;
    
    if (dir != ForwardScanDirection)
      elog(ERROR, "SpGiST only supports forward scan direction");

    if (!so->qual_ok)
      return false;
  
    if (so->firstCall)
    {
      // elog(NOTICE, "my_spggettuple ----------- 1");
      /* Begin the scan by processing the root page */
      SPGISTSearchItem fakeItem;
      // GISTSearchItem fakeItem;

      pgstat_count_index_scan(scan->indexRelation);

      so->firstCall = false;
      // so->curPageData = so->nPageData = 0;
      scan->xs_itup = NULL;
      
      fakeItem.blkno = SPGIST_ROOT_BLKNO;
      BlockIdSet(&(fakeItem.ptr.ip_blkid), SPGIST_ROOT_BLKNO);
      
      fakeItem.ptr.ip_posid = FirstOffsetNumber;
      fakeItem.level = 0;
      fakeItem.P_min.x = 0.0;
      fakeItem.P_min.y = 0.0;
      
      // elog(NOTICE, "my_spggettuple ----------- 2");
      // spgistScanPage2(scan, &fakeItem, &LeafReached);
      spgistScanPage3(scan, &fakeItem, &LeafReached);
      // elog(NOTICE, "my_spggettuple ----------- 3");
    }

  
    // elog(NOTICE, "my_spggettuple ----------- 4");
    /* Must fetch tuples in strict distance order */
    return my_getNextNearest(scan);
  }

  return false;
}
#define SPTEST(f, x, y) \
  DatumGetBool(DirectFunctionCall2(f, PointPGetDatum(x), PointPGetDatum(y)))

/*
 * Determine which quadrant a point falls into, relative to the centroid.
 *
 * Quadrants are identified like this:
 *
 *   4  |  1
 *  ----+-----
 *   3  |  2
 *
 * Points on one of the axes are taken to lie in the lowest-numbered
 * adjacent quadrant.
 */
static int16
getQuadrant(Point *centroid, Point *tst)
{
  // elog(NOTICE, "getQuadrant -----  centroid(%f,%f) - tst(%f,%f)", 
  //                                 centroid->x , centroid->y ,
  //                                 tst->x , tst->y);
  if ((SPTEST(point_above, tst, centroid) ||
     SPTEST(point_horiz, tst, centroid)) &&
    (SPTEST(point_right, tst, centroid) ||
     SPTEST(point_vert, tst, centroid)))
    return 1;

  if (SPTEST(point_below, tst, centroid) &&
    (SPTEST(point_right, tst, centroid) ||
     SPTEST(point_vert, tst, centroid)))
    return 2;

  if ((SPTEST(point_below, tst, centroid) ||
     SPTEST(point_horiz, tst, centroid)) &&
    SPTEST(point_left, tst, centroid))
    return 3;

  if (SPTEST(point_above, tst, centroid) &&
    SPTEST(point_left, tst, centroid))
    return 4;

  elog(ERROR, "getQuadrant: impossible case");
  return 0;
}







static void spgistScanPage3(IndexScanDesc scan, SPGISTSearchItem *pageItem, bool *LeafReached)
{
  // elog(NOTICE, "spgistScanPage3 ----------- start");

  // my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
  mySpGistScanOpaque so = (mySpGistScanOpaque) scan->opaque;

  bool    recheck;
  // bool    recheck_distances;

  BlockNumber blkno;
  OffsetNumber offset;
  Page    page;
  Buffer buffer = InvalidBuffer;
  bool    isnull;
  // MemoryContext oldCtx;
  Relation  index = scan->indexRelation;
  FmgrInfo   *procinfo;

  //DEBUG
  // printf("\nspgistScanPage3 ----------- start\n");
  // printf("t_numscans =  %d\n", index->pgstat_info->t_counts.t_numscans);

  // printf("t_tuples_returned =  %d\n", index->pgstat_info->t_counts.t_tuples_returned);
  // printf("t_tuples_fetched =  %d\n", index->pgstat_info->t_counts.t_tuples_fetched);
  // printf("t_delta_live_tuples =  %d\n", index->pgstat_info->t_counts.t_delta_live_tuples);
  // printf("t_delta_dead_tuples =  %d\n", index->pgstat_info->t_counts.t_delta_dead_tuples);
  // printf("t_changed_tuples =  %d\n", index->pgstat_info->t_counts.t_changed_tuples);
  // printf("t_blocks_fetched =  %d\n", index->pgstat_info->t_counts.t_blocks_fetched);
  // printf("t_blocks_hit =  %d\n", index->pgstat_info->t_counts.t_blocks_hit);

  //DEBUG

  // so->nPageData = so->curPageData = 0;
  scan->xs_itup = NULL;   /* might point into pageDataCxt */

redirect:  
  
  /* Check for interrupts, just in case of infinite loop */
  CHECK_FOR_INTERRUPTS();

  blkno = ItemPointerGetBlockNumber(&pageItem->ptr);
  offset = ItemPointerGetOffsetNumber(&pageItem->ptr);

  if (buffer == InvalidBuffer)
  {
    buffer = ReadBuffer(index, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
  }
  else if (blkno != BufferGetBlockNumber(buffer))
  {
    UnlockReleaseBuffer(buffer);
    buffer = ReadBuffer(index, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
  }

  
  page = BufferGetPage(buffer);
  TestForOldSnapshot(scan->xs_snapshot, index, page);

  isnull = SpGistPageStoresNulls(page) ? true : false;
    
  //DEBUG
  // printf("------- 2 ---------\n");
  // printf("t_numscans =  %d\n", index->pgstat_info->t_counts.t_numscans);

  // printf("t_tuples_returned =  %d\n", index->pgstat_info->t_counts.t_tuples_returned);
  // printf("t_tuples_fetched =  %d\n", index->pgstat_info->t_counts.t_tuples_fetched);
  // printf("t_delta_live_tuples =  %d\n", index->pgstat_info->t_counts.t_delta_live_tuples);
  // printf("t_delta_dead_tuples =  %d\n", index->pgstat_info->t_counts.t_delta_dead_tuples);
  // printf("t_changed_tuples =  %d\n", index->pgstat_info->t_counts.t_changed_tuples);
  // printf("t_blocks_fetched =  %d\n", index->pgstat_info->t_counts.t_blocks_fetched);
  // printf("t_blocks_hit =  %d\n", index->pgstat_info->t_counts.t_blocks_hit);
  
  //DEBUG


  // elog(NOTICE, "spgistScanPage3 ----------- 1");
  if (SpGistPageIsLeaf(page))
  {

    // elog(NOTICE, "spgistScanPage3 ----------- 2");
    SpGistLeafTuple leafTuple;
    OffsetNumber max = PageGetMaxOffsetNumber(page);
    recheck = false;
      
    if (SpGistBlockIsRoot(blkno))
    {
      /* When root is a leaf, examine all its tuples */
      //TODO : copy from spgwalk() in spgscan.c
      // elog(NOTICE, "spgistScanPage3 ----------- 3");
    }
    else
    {
      // elog(NOTICE, "spgistScanPage3 ----------- 4");
      /* Normal case: just examine the chain we arrived at */
      while (offset != InvalidOffsetNumber)
      {
        
        Assert(offset >= FirstOffsetNumber && offset <= max);
        leafTuple = (SpGistLeafTuple)
          PageGetItem(page, PageGetItemId(page, offset));
        
        if (leafTuple->tupstate != SPGIST_LIVE)
        {
          // DEBUG
          printf("\n\nLeaf tuple is NOT LIVE\n");
          //DEBUG
          if (leafTuple->tupstate == SPGIST_REDIRECT)
          {
            // DEBUG
            printf("Leaf tuple Redirect\n");
            //DEBUG

            /* redirection tuple should be first in chain */
            Assert(offset == ItemPointerGetOffsetNumber(&pageItem->ptr));
            /* transfer attention to redirect point */
            pageItem->ptr = ((SpGistDeadTuple) leafTuple)->pointer;
            Assert(ItemPointerGetBlockNumber(&pageItem->ptr) != SPGIST_METAPAGE_BLKNO);
            goto redirect;
          }
          if (leafTuple->tupstate == SPGIST_DEAD)
          {
            // DEBUG
            printf("Leaf tuple is DEAD\n");
            //DEBUG

            /* dead tuple should be first in chain */
            Assert(offset == ItemPointerGetOffsetNumber(&pageItem->ptr));
            /* No live entries on this page */
            Assert(leafTuple->nextOffset == InvalidOffsetNumber);
            break;
          }
          /* We should not arrive at a placeholder */
          elog(ERROR, "unexpected SPGiST tuple state: %d",
             leafTuple->tupstate);
        }

        Assert(ItemPointerIsValid(&leafTuple->heapPtr));

        /* create the spGistSearchItem to be inserted in the queue*/
        SPGISTSearchItem *item;
        
        // oldCtx = MemoryContextSwitchTo(so->queueCxt);

        item = palloc(SizeOfSPGISTSearchItem(scan->numberOfOrderBys));

        /* Creating heap-tuple GISTSearchItem */
        item->blkno = InvalidBlockNumber;
        item->data.heap.heapPtr = leafTuple->heapPtr;
        item->data.heap.recheck = false;
        item->data.heap.recheckDistances = false;
        item->ptr = leafTuple->heapPtr;
        item->level = pageItem->level+1;
        item->P_center = pageItem->P_center;
        item->P_min.x = pageItem->P_min.x;
        item->P_min.y = pageItem->P_min.y;
        item->P_max.x = pageItem->P_max.x;
        item->P_max.y = pageItem->P_max.y;

        /* Compute the distance */
        Point *p = DatumGetPointP(SGLTDATUM(leafTuple, &so->state));
        
        //DEBUG
        // if(blkno == 133)
        //   printf("[%d,%d,%d] - (%f,%f) , (%f,%f), (%f,%f)\n" , 
        //     blkno , offset , item->level , 
        //      item->P_center.x , item->P_center.y , 
        //      item->P_min.x , item->P_min.y , 
        //      item->P_max.x , item->P_max.y);
        //DEBUG

        // my_computeDistance(scan, item, 0 , true, p);

        //===============================
        // call the built in distance function in the index relation
        
        procinfo = index_getprocinfo(index, 1, SPGIST_DISTANCE_POINT_PROC); // attribute number ??? 
        FunctionCall5Coll(procinfo,
                  index->rd_indcollation[0], //??
                  PointerGetDatum(scan),
                  PointerGetDatum(item),
                  Int16GetDatum(0),
                  BoolGetDatum(true),
                  PointPGetDatum(p));
        //===============================

        /* Insert it into the queue using new distance data */
        memcpy(item->distances, so->distances,
             sizeof(double) * scan->numberOfOrderBys);

        pairingheap_add(so->queue, &item->phNode);
        

        offset = leafTuple->nextOffset;
      }
      // MemoryContextSwitchTo(oldCtx);
      // MemoryContextReset(so->tempCxt);
    }

    //DEBUG
    // printf("blkno = %d  ,  dist = %f\n" , blkno , so->distances[0]);
    //DEBUG
  }
  else  /* page is inner */
  {
    // elog(NOTICE, "spgistScanPage3 ----------- 5");
    SpGistInnerTuple innerTuple;
    // spgInnerConsistentIn in;
    // spgInnerConsistentOut out;
    // FmgrInfo   *procinfo;
    // SpGistNodeTuple *nodes;
    SpGistNodeTuple node;
    int     i;
    

    *LeafReached = false;

    innerTuple = (SpGistInnerTuple) PageGetItem(page,
                      PageGetItemId(page, offset));

    if (innerTuple->tupstate != SPGIST_LIVE)
    {
      // DEBUG
      printf("\n\nLeaf tuple is NOT LIVE\n");
      //DEBUG
      if (innerTuple->tupstate == SPGIST_REDIRECT)
      {
        // DEBUG
        printf("Leaf tuple Redirect\n");
        //DEBUG

        /* transfer attention to redirect point */
        pageItem->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
        Assert(ItemPointerGetBlockNumber(&pageItem->ptr) != SPGIST_METAPAGE_BLKNO);
        goto redirect;
      }
      elog(ERROR, "unexpected SPGiST tuple state: %d",
         innerTuple->tupstate);
    }


    int which = 0; /* which quadrant this inner node lies in */
    
    Point* center = DatumGetPointP(SGITDATUM(innerTuple, &so->state));
    Point min, max ; /* corner points of this inner node*/
    
    // elog(NOTICE, "spgistScanPage3 ----------- 6");

    if(pageItem->level == 0) // root Block
    {
      min.x = min.y = 0.0;
      max.x = ceil(2 * center->x);
      max.y = ceil(2 * center->y);
      // elog(NOTICE, "spgistScanPage3 ----------- 7");
    }
    else
    {
      /* get which quadrant this inner node lies in */
      which = getQuadrant( &pageItem->P_center , center);
      // elog(NOTICE, "spgistScanPage3 ----------- 8");
      switch(which)
      {
        case 1:
          min.x = floor(pageItem->P_center.x);
          min.y = floor(pageItem->P_center.y);
          max.x = ceil(pageItem->P_max.x);
          max.y = ceil(pageItem->P_max.y);
        break;
        case 2:
          min.x = floor(pageItem->P_center.x);
          min.y = floor(pageItem->P_min.y);
          max.x = ceil(pageItem->P_max.x);
          max.y = ceil(pageItem->P_center.y);
        break;
        case 3:
          min.x = floor(pageItem->P_min.x);
          min.y = floor(pageItem->P_min.y);
          max.x = ceil(pageItem->P_center.x);
          max.y = ceil(pageItem->P_center.y);
        break;
        case 4:
          min.x = floor(pageItem->P_min.x);
          min.y = floor(pageItem->P_center.y);
          max.x = ceil(pageItem->P_center.x);
          max.y = ceil(pageItem->P_max.y);
        break;

        default:
        break;

      }
    }
    


    SGITITERATE(innerTuple, i, node)
    {
      
      if (ItemPointerIsValid(&node->t_tid))
      {

        /* create the spGistSearchItem to be inserted in the queue*/
        SPGISTSearchItem *item;
        item = palloc(SizeOfSPGISTSearchItem(scan->numberOfOrderBys));

         /* Creating heap-tuple GISTSearchItem */
        item->blkno = ItemPointerGetBlockNumber(&node->t_tid);
        item->data.heap.heapPtr = node->t_tid;
        item->ptr = node->t_tid;
        
        item->level = pageItem->level+1;
        item->P_center = *center;
        item->P_min.x = min.x;
        item->P_min.y = min.y;
        item->P_max.x = max.x;
        item->P_max.y = max.y;

        
        /* Compute the distance : Assume that nodes are ordered 
           (i=0 -> quad 1 , i=1 -> quad=2 ... etc)
            default that this node is inner, and then when it 
            comes back to scan page I'll figure out that it's a leaf page */

        // elog(NOTICE, "spgistScanPage3 ----------- 9");
        // my_computeDistance(scan, item, i+1, false ,NULL ); 

        //===============================
        // call the built in distance function in the index relation
        Point * ppp = NULL;
        procinfo = index_getprocinfo(index, 1, SPGIST_DISTANCE_POINT_PROC); // attribute number ??? 
        FunctionCall5Coll(procinfo,
                  index->rd_indcollation[0], //??
                  PointerGetDatum(scan),
                  PointerGetDatum(item),
                  Int16GetDatum(i+1),
                  BoolGetDatum(false),
                  PointPGetDatum(ppp));
        //===============================
        // elog(NOTICE, "spgistScanPage3 ----------- 10");
        /* Insert it into the queue using new distance data */
        memcpy(item->distances, so->distances,
             sizeof(double) * scan->numberOfOrderBys);

        pairingheap_add(so->queue, &item->phNode);
        
      }
    }
    // MemoryContextSwitchTo(oldCtx);
    // MemoryContextReset(so->tempCxt);
  }

  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);
}




// static bool
// spgistindex_keytest(IndexScanDesc scan,
//           IndexTuple tuple,
//           Page page,
//           OffsetNumber offset,
//           bool *recheck_p,
//           bool *recheck_distances_p)
// {
//   // GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
//   // GISTSTATE  *giststate = so->giststate;
  
//   my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
//   // SpGistState *spgistate = &(so->state);

//   ScanKey   key = scan->keyData;
//   int     keySize = scan->numberOfKeys;
//   double     *distance_p;
//   // Relation  r = scan->indexRelation;

//   *recheck_p = false;
//   *recheck_distances_p = false;

//   /* now let's compute the distances */
//   key = scan->orderByData;
//   distance_p = so->distances;
//   keySize = scan->numberOfOrderBys;
//   while (keySize > 0)
//   {
//     Datum   datum;
//     bool    isNull;

//     // datum = index_getattr(tuple,
//     //             key->sk_attno,
//     //             so->indexTupDesc,
//     //             &isNull);

//       if(!SpGistPageIsLeaf(page))
//     { ///Inner tuple
//       SpGistInnerTuple innerTuple;
//       innerTuple = (SpGistInnerTuple) PageGetItem(page, PageGetItemId(page, offset));
      
//       if (innerTuple->tupstate != SPGIST_LIVE)
//       {
//         isNull = true;
//       }
//       datum = SGITDATUM(innerTuple, &so->state);

//       //centroid = DatumGetPointP(prefixDatum);
//     }
//     else
//     { // Leaf Tuple
//       SpGistLeafTuple leafTuple;
//       leafTuple = (SpGistLeafTuple) PageGetItem(page, PageGetItemId(page, offset));
      
//       if (leafTuple->tupstate != SPGIST_LIVE)
//       {
//         isNull = true;
//       }
//       datum = SGLTDATUM(leafTuple, &so->state);
//       // centroid = DatumGetPointP(datumValue);
//     }


//     if ((key->sk_flags & SK_ISNULL) || isNull)
//     {
//       /* Assume distance computes as null and sorts to the end */
//       *distance_p = get_float8_infinity();
//     }
//     else
//     {
//       Datum   dist;
//       bool    recheck;
      
      
//        * Call the Distance function to evaluate the distance.  The
//        * arguments are the index datum (as a GISTENTRY*), the comparison
//        * datum, the ordering operator's strategy number and subtype from
//        * pg_amop, and the recheck flag.
//        *
//        * (Presently there's no need to pass the subtype since it'll
//        * always be zero, but might as well pass it for possible future
//        * use.)
//        *
//        * If the function sets the recheck flag, the returned distance is
//        * a lower bound on the true distance and needs to be rechecked.
//        * We initialize the flag to 'false'.  This flag was added in
//        * version 9.5; distance functions written before that won't know
//        * about the flag, but are expected to never be lossy.
       
//       recheck = false;
//       Point *queryPoint = DatumGetPointP(key->sk_argument);
//        // Point *currPoint;
//       // BOX *boundingBox;
//       //datum // this is the attribute point
//       //if it's leaf , distance between two points , recheck = false;
//       bool isLeaf = true;
//       if(!SpGistPageIsLeaf(page))
//       {
//         // boundingBox = DatumGetBoxP(datum);
//         recheck = true;
//         isLeaf = false;
//       }
//       //if it's not a leaf, compute the distance from the point and a abounding box for the block
//       //                  && recheck = true;

//       // dist =  computeDistance(isLeaf, DatumGetBoxP(datum) , queryPoint ); // distance function
//       dist = point_point_distance(DatumGetPointP(datum), queryPoint);
//       // FunctionCall5Coll(&key->sk_func,
//       //              key->sk_collation,
//       //              PointerGetDatum(&de),
//       //              key->sk_argument,
//       //              Int16GetDatum(key->sk_strategy),
//       //              ObjectIdGetDatum(key->sk_subtype),
//       //              PointerGetDatum(&recheck));
      
//       *recheck_distances_p |= recheck;
//       *distance_p = dist;//DatumGetFloat8(dist);
//     }

//     key++;
//     distance_p++;
//     keySize--;
//   }

//   return true;
// }

//compute the distance between the tuple and each query point in orderyBy key
// static bool
// spgistindex_keytest_computeDistance(IndexScanDesc scan, SpGistNodeTuple tuple,
//           Page page,
//           OffsetNumber offset,
//           bool *recheck_p,
//           bool *recheck_distances_p)
// {
//   my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
//   // SpGistState *spgistate = &(so->state);

//   ScanKey   key = scan->orderByData;
//   int     keySize = scan->numberOfOrderBys;
//   double     *distance_p = so->distances;
//   // Relation  r = scan->indexRelation;
//   bool isNull;

//   *recheck_p = false;
//   *recheck_distances_p = false;

//   /* ========================================*/
//   /* TODO: This part should be removed, opening the page and read the buffer for each tuple to get the
//       center point of it so I can compute its distance from the query point is overhead*/
//   Buffer buffer = 0;
//   if(tuple)
//   {
//     BlockNumber blkno;
    
//     // bool    isnull;
//     Relation  index = scan->indexRelation;

//     blkno = ItemPointerGetBlockNumber(&tuple->t_tid);
//     offset = ItemPointerGetOffsetNumber(&tuple->t_tid);

//     buffer = ReadBuffer(index, blkno);
//     LockBuffer(buffer, BUFFER_LOCK_SHARE);
    
//     page = BufferGetPage(buffer);
//     TestForOldSnapshot(scan->xs_snapshot, index, page);
//   }
  
//   /* ==================================================*/
  
//   isNull = SpGistPageStoresNulls(page) ? true : false;
//   while (keySize > 0)
//   {
//     Datum datum;
//     bool isLeaf = true;
//     bool recheck = false;

    
    
//     if(!SpGistPageIsLeaf(page))
//     { ///Inner tuple
//       SpGistInnerTuple innerTuple;
//       innerTuple = (SpGistInnerTuple) PageGetItem(page, PageGetItemId(page, offset));
      
//       if (innerTuple->tupstate != SPGIST_LIVE)
//       {
//         isNull = true;
//       }
//       datum = SGITDATUM(innerTuple, &so->state);
//       isLeaf = false;
//       recheck = true;
//     }
//     else
//     { // Leaf Tuple
//       SpGistLeafTuple leafTuple;
//       leafTuple = (SpGistLeafTuple) PageGetItem(page, PageGetItemId(page, offset));
      
//       if (leafTuple->tupstate != SPGIST_LIVE)
//       {
//         isNull = true;
//       }
//       datum = SGLTDATUM(leafTuple, &so->state);
//     }


//     if ((key->sk_flags & SK_ISNULL) || isNull)
//     {
//       /* Assume distance computes as null and sorts to the end */
//       *distance_p = get_float8_infinity();
//     }
//     else
//     {
//       double   dist;
      
//       Point *queryPoint = DatumGetPointP(key->sk_argument);
      
//       dist = point_point_distance(DatumGetPointP(datum), queryPoint);
      
//       *recheck_distances_p |= recheck;
//       *distance_p = dist;//DatumGetFloat8(dist);
//     }

//     key++;
//     distance_p++;
//     keySize--;
//   }

//   if (buffer != InvalidBuffer)
//     UnlockReleaseBuffer(buffer);
//       // LockBuffer(buffer, BUFFER_LOCK_UNLOCK);  
//   return true;
// }

//compute the distance between the tuple and each query point in orderyBy key
// static bool
// my_computeDistance(IndexScanDesc scan, SPGISTSearchItem * item, int which, bool isLeaf , Point * leafVal)
// {
//   my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
 
//   ScanKey   key = scan->orderByData;
//   int     keySize = scan->numberOfOrderBys;
//   double     *distance_p = so->distances;
//   bool isNull = false;

//   item->data.heap.recheck = false;
//   item->data.heap.recheckDistances = false;
  
//   while (keySize > 0)
//   {
//     bool recheck = false;

//     if ((key->sk_flags & SK_ISNULL) || isNull)
//     {
//       /* Assume distance computes as null and sorts to the end */
//       *distance_p = get_float8_infinity();
//     }
//     else
//     {
//       double   dist;
//       Point *queryPoint = DatumGetPointP(key->sk_argument);
//       BOX box;
      
//       if(isLeaf) 
//         {
//           box.low = box.high = *(leafVal);
//         }
//       else
//       {
//         recheck = true;
        
//         switch(which)
//         {
//           case 1:
//             box.low.x = item->P_center.x;
//             box.low.y = item->P_center.y;
//             box.high.x = item->P_max.x;
//             box.high.y = item->P_max.y;
//           break;
//           case 2:
//             box.low.x = item->P_center.x;
//             box.low.y = item->P_min.y;
//             box.high.x = item->P_max.x;
//             box.high.y = item->P_center.y;
//           break;
//           case 3:
//             box.low.x = item->P_min.x;
//             box.low.y = item->P_min.y;
//             box.high.x = item->P_center.x;
//             box.high.y = item->P_center.y;
//           break;
//           case 4:
//             box.low.x = item->P_min.x;
//             box.low.y = item->P_center.y;
//             box.high.x = item->P_center.x;
//             box.high.y = item->P_max.y;
//           break;
//           default:
//           break;
//         }
//       }
      
//       // elog(NOTICE, "my_computeDistance ----------- 1");
//       dist = computeDistance( isLeaf,  &box, queryPoint);
//       // elog(NOTICE, "my_computeDistance ----------- 2");

//       item->data.heap.recheckDistances |= recheck;
//       *distance_p = dist;
//     }

//     key++;
//     distance_p++;
//     keySize--;
//   }

//   return true;
// }

/*
 * Fetch next heap tuple in an ordered search
  */
static bool
my_getNextNearest(IndexScanDesc scan)
{
  // my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
  mySpGistScanOpaque so = (mySpGistScanOpaque) scan->opaque;
  bool    res = false;
  int     i;
  bool LeafReached = false;
  
  if (scan->xs_itup)
  {
    /* free previously returned tuple */
    pfree(scan->xs_itup);
    scan->xs_itup = NULL;
  }

  do
  {
    SPGISTSearchItem *item = getNextSPGISTSearchItem(so);
    if (!item)
      break;

    if (SPGISTSearchItemIsHeap(*item))
    {
      /* found a heap item at currently minimal distance */
      scan->xs_ctup.t_self = item->data.heap.heapPtr;
      scan->xs_recheck = item->data.heap.recheck;
      scan->xs_recheckorderby = item->data.heap.recheckDistances;
      
      for (i = 0; i < scan->numberOfOrderBys; i++)
      {
        if (so->orderByTypes[i] == FLOAT8OID)
        {
#ifndef USE_FLOAT8_BYVAL
          /* must free any old value to avoid memory leakage */
          if (!scan->xs_orderbynulls[i])
            pfree(DatumGetPointer(scan->xs_orderbyvals[i]));
#endif
          scan->xs_orderbyvals[i] = Float8GetDatum(item->distances[i]);
          scan->xs_orderbynulls[i] = false;
        }
        else if (so->orderByTypes[i] == FLOAT4OID)
        {
          /* convert distance function's result to ORDER BY type */
#ifndef USE_FLOAT4_BYVAL
           must free any old value to avoid memory leakage 
          if (!scan->xs_orderbynulls[i])
            pfree(DatumGetPointer(scan->xs_orderbyvals[i]));
#endif
          scan->xs_orderbyvals[i] = Float4GetDatum((float4) item->distances[i]);
          scan->xs_orderbynulls[i] = false;
        }
        else
        {
          /*
           * If the ordering operator's return value is anything
           * else, we don't know how to convert the float8 bound
           * calculated by the distance function to that.  The
           * executor won't actually need the order by values we
           * return here, if there are no lossy results, so only
           * insist on converting if the *recheck flag is set.
           */
          if (scan->xs_recheckorderby)
            elog(ERROR, "GiST operator family's FOR ORDER BY operator must return float8 or float4 if the distance function is lossy");
          scan->xs_orderbynulls[i] = true;
        }
      }

      /* in an index-only scan, also return the reconstructed tuple. */
      if (scan->xs_want_itup)
        scan->xs_itup = item->data.heap.ftup;
      res = true;
    }
    else
    {
      /* visit an index page, extract its items into queue */
      CHECK_FOR_INTERRUPTS();

      spgistScanPage3(scan, item, &LeafReached);
    }

    pfree(item);
  } while (!res);

  return res;
}


/*
 * Extract next item (in order) from search queue
 *
 * Returns a GISTSearchItem or NULL.  Caller must pfree item when done with it.
 */
static SPGISTSearchItem *
getNextSPGISTSearchItem(mySpGistScanOpaque so)
{
  SPGISTSearchItem *item;

  if (!pairingheap_is_empty(so->queue))
  {
    item = (SPGISTSearchItem *) pairingheap_remove_first(so->queue);
  }
  else
  {
    /* Done when both heaps are empty */
    item = NULL;
  }

  /* Return item; caller is responsible to pfree it */
  return item;
}


/* 
 * Compute boudning boxes for each page 
*/

#define FLOAT8_LT(a,b)  (float8_cmp_internal(a, b) < 0)
#define FLOAT8_GT(a,b)  (float8_cmp_internal(a, b) > 0)

/*
 * Increase BOX b to include addon.
 */
static void
adjustBox(BOX *b, const BOX *addon)
{
  if (FLOAT8_LT(b->high.x, addon->high.x))
    b->high.x = addon->high.x;
  if (FLOAT8_GT(b->low.x, addon->low.x))
    b->low.x = addon->low.x;
  if (FLOAT8_LT(b->high.y, addon->high.y))
    b->high.y = addon->high.y;
  if (FLOAT8_GT(b->low.y, addon->low.y))
    b->low.y = addon->low.y;
}


void start_Compute_BoundingBox(Relation index, Oid indexid)
{
  // elog(NOTICE, "start_Compute_BoundingBox ... start");
  
  ItemPointer node = palloc(sizeof(ItemPointer));
  ItemPointerSet(node, SPGIST_ROOT_BLKNO, FirstOffsetNumber);
  
  // Compute_BoundingBox(node, index);
  // Compute_BoundingBoxes(index, indexid);
  // elog(NOTICE, "start_Compute_BoundingBox ... Finish");
}

BOX Compute_BoundingBox(ItemPointer itptr, Relation index )
{
  Page page;
  BlockNumber blkno;
  OffsetNumber offset;
  Buffer buffer = InvalidBuffer;
  SpGistPageOpaque opaque;
  BOX *BBox = NULL;

  blkno = ItemPointerGetBlockNumber(itptr);
  offset = ItemPointerGetOffsetNumber(itptr);


  buffer = ReadBuffer(index, blkno);
  LockBuffer(buffer, BUFFER_LOCK_SHARE);
  
  /* else new pointer points to the same page, no work needed */

  page = BufferGetPage(buffer);


  opaque = SpGistPageGetOpaque(page);

  /* Check for interrupts, just in case of infinite loop */
  CHECK_FOR_INTERRUPTS();

  if (SpGistPageIsLeaf(page))
  {
    SpGistLeafTuple leafTuple;
    OffsetNumber max = PageGetMaxOffsetNumber(page);
    
    if (SpGistBlockIsRoot(blkno))
    {
      //DEBUG
       printf("\n\nPage is Leaf and Root .... blkno %d , maxoff = %d\n", blkno , max);
      //DEBUG

      /* When root is a leaf, examine all its tuples */
      for (offset = FirstOffsetNumber; offset <= max; offset++)
      {
        leafTuple = (SpGistLeafTuple)
          PageGetItem(page, PageGetItemId(page, offset));
        if (leafTuple->tupstate != SPGIST_LIVE)
        {
          /* all tuples on root should be live */
          elog(ERROR, "unexpected SPGiST tuple state: %d",
             leafTuple->tupstate);
        }

        Assert(ItemPointerIsValid(&leafTuple->heapPtr));
        
        /* TODO: get the BoundingBox for this leaf page 
                 and assign it to the opaque  */
        //assign the leaf point in a box
        Point *p = DatumGetPointP(PointerGetDatum(SGLTDATAPTR(leafTuple)));
        BOX *cur = (BOX *) palloc(sizeof(BOX));
        cur->high = cur->low = *p;

        if(!BBox)
        { 
          BBox = (BOX *) palloc(sizeof(BOX));
          memcpy((void *) BBox, (void *) cur, sizeof(BOX));
        }
        else
          adjustBox(BBox, cur);

        //DEBUG
         printf(" curBox low(%f, %f) , high(%f, %f) \n", cur->low.x, cur->low.y , cur->high.x, cur->high.y);
         printf(" BBox low(%f, %f) , high(%f, %f) \n", BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
        //DEBUG
      }
      //DEBUG
       printf(" BBox low ( %f , %f ) - high ( %f , %f ) \n", BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
      //DEBUG
      // opaque->BBox.high = BBox->high; 
      // opaque->BBox.low = BBox->low;
      // return *BBox;
    }
    else
    {
      //DEBUG
       printf("\n\nPage is Leaf .... blkno %d - offset = %d  - maxoff = %d\n", blkno, offset , max);
      //DEBUG

      /* Normal case: just examine the chain we arrived at */
      while (offset != InvalidOffsetNumber)
      {
        Assert(offset >= FirstOffsetNumber && offset <= max);
        leafTuple = (SpGistLeafTuple)
          PageGetItem(page, PageGetItemId(page, offset));
        if (leafTuple->tupstate != SPGIST_LIVE)
        {
          if (leafTuple->tupstate == SPGIST_REDIRECT)
          {
            elog(NOTICE, "\n\nLeafTuple is SPGIST_REDIRECT\n\n");
            break;
            /* redirection tuple should be first in chain */
            // Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
            /* transfer attention to redirect point */
            // stackEntry->ptr = ((SpGistDeadTuple) leafTuple)->pointer;
          //  Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
          //  goto redirect;
          }
          if (leafTuple->tupstate == SPGIST_DEAD)
          {
            /* dead tuple should be first in chain */
            // Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
            /* No live entries on this page */
            Assert(leafTuple->nextOffset == InvalidOffsetNumber);
            break;
          }
          /* We should not arrive at a placeholder */
          elog(ERROR, "unexpected SPGiST tuple state: %d",
             leafTuple->tupstate);
        }

        Assert(ItemPointerIsValid(&leafTuple->heapPtr));
        
        //TODO: get the Bounding Box for all the points that are in this page leaf
        //assign the leaf point in a box
        Point *p = DatumGetPointP(PointerGetDatum(SGLTDATAPTR(leafTuple)));
        BOX *cur = (BOX *) palloc(sizeof(BOX));
        cur->high = cur->low = *p;

        if(!BBox)
        { 
          BBox = (BOX *) palloc(sizeof(BOX));
          memcpy((void *) BBox, (void *) cur, sizeof(BOX));
        }
        else
          adjustBox(BBox, cur);

        //DEBUG
        // printf(" curBox low(%f, %f) , high(%f, %f) \n", cur->low.x, cur->low.y , cur->high.x, cur->high.y);
        // printf(" BBox low(%f, %f) , high(%f, %f) \n", BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
        //DEBUG

        offset = leafTuple->nextOffset;
      }
      //DEBUG
      // printf("[ %d ]  - BBox low ( %f , %f ) - high ( %f , %f ) \n",blkno, BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
      //DEBUG
      // opaque->BBox.high = BBox->high; 
      // opaque->BBox.low = BBox->low;
      // return *BBox;
    }
  }
  else /* page is inner */
  {
    //DEBUG
    printf("\n\nPage is inner .... [blkno , offset ] =  [ %d , %d ] \n", blkno , offset);
    //DEBUG

    SpGistInnerTuple innerTuple;
    SpGistNodeTuple node;
    SpGistNodeTuple nodes[4] ;
    int     i;
    
    innerTuple = (SpGistInnerTuple) PageGetItem(page,
                      PageGetItemId(page, offset));

    if (innerTuple->tupstate != SPGIST_LIVE)
    {
      if (innerTuple->tupstate == SPGIST_REDIRECT)
      {
        elog(NOTICE, "\n\nLeafTuple is SPGIST_REDIRECT\n\n");
        /* transfer attention to redirect point */
        // stackEntry->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
        // Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
        // goto redirect;
      }
      else
      elog(ERROR, "unexpected SPGiST tuple state: %d",
         innerTuple->tupstate);
    }

    BOX cur[4]; //= (BOX **) palloc(sizeof(BOX *) * 4);

    //DEBUG
    if(blkno == 8 && offset == 286)
      elog(NOTICE, " ...llllllllllllllll");
    //DEBUG
    SGITITERATE(innerTuple, i, node)
    {
      if (ItemPointerIsValid(&node->t_tid))
      {
        nodes[i] = node;
        printf("node[%d]  =  [blkno , offset] =  [ %d , %d ]\n ", i , ItemPointerGetBlockNumber(&node->t_tid),
                                                                      ItemPointerGetOffsetNumber(&node->t_tid));
        // cur[i] = Compute_BoundingBox(&node->t_tid, index);
      }
    }

    // if (buffer != InvalidBuffer)
    // UnlockReleaseBuffer(buffer);
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

    //compute the bounding boxes of the 4 children
    for(i=0; i<4; i++)
    {
      cur[i] = Compute_BoundingBox(&nodes[i]->t_tid, index);
    }

    // union the boxes
    for(i=0; i<4; i++)
    {
      if(!BBox)
      { 
        BBox = (BOX *) palloc(sizeof(BOX));
        memcpy((void *) BBox, (void *) &cur[i], sizeof(BOX));
      }
      else
        adjustBox(BBox, &cur[i]);

      //DEBUG
      // printf(" curBox low ( %f , %f ) - high ( %f , %f ) \n", cur[i].low.x, cur[i].low.y , cur[i].high.x, cur[i].high.y);
      // printf(" BBox low ( %f , %f ) - high ( %f , %f ) \n", BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
      //DEBUG

    }
    //DEBUG
    // printf("[ %d , %d ]\t-\tBBox low ( %f , %f ) - high ( %f , %f ) \n", blkno , offset, BBox->low.x, BBox->low.y , BBox->high.x, BBox->high.y);
    //DEBUG
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    // opaque->BBox.high = BBox->high; 
    // opaque->BBox.low = BBox->low;
    
  }

  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);

  return *BBox;
}

typedef struct ScanStackEntry
{
  Datum   reconstructedValue;   /* value reconstructed from parent */
  void     *traversalValue; /* opclass-specific traverse value */
  int     level;      /* level of items on this page */
  ItemPointerData ptr;    /* block and offset to scan from */
  
} ScanStackEntry;

/* iterate over the index and scan all the pages 
   and print the points in the leaf nodes and the 
   centroids point of each quad */
void
Compute_BoundingBoxes(Relation index)
{
  printf("\n------------------------------------ Compute_BoundingBoxes start\n");
  Buffer buffer = InvalidBuffer;
  // MemoryContext tempCxt, oldCtx;
  bool scanWholeIndex = true;
  List *scanStack = NIL;
  ScanStackEntry * rootEntry;
  
  
  rootEntry = palloc(sizeof(ScanStackEntry));
  rootEntry->level = 0;
  rootEntry->reconstructedValue = (Datum) 0;
  rootEntry->traversalValue = NULL;


  ItemPointerSet(&rootEntry->ptr, SPGIST_ROOT_BLKNO, FirstOffsetNumber);
    scanStack = lappend(scanStack, rootEntry);

  // printf("Index oid: %d", indexid);
  // printf("------------ Index Relation:\n");
  // pprint (index);
  // printf("\n------------------------------------Start\n");

  //debug
    int pgcnt = 0; 
    //debug
  while (scanWholeIndex )
  {
    ScanStackEntry *stackEntry;
    BlockNumber blkno;
    OffsetNumber offset;
    Page    page;
    bool    isnull;

    /* Pull next to-do item from the list */
    if (scanStack == NIL)
      break;        /* there are no more pages to scan */

    stackEntry = (ScanStackEntry *) linitial(scanStack);
    scanStack = list_delete_first(scanStack);

redirect:
    /* Check for interrupts, just in case of infinite loop */
    CHECK_FOR_INTERRUPTS();

    blkno = ItemPointerGetBlockNumber(&stackEntry->ptr);
    offset = ItemPointerGetOffsetNumber(&stackEntry->ptr);

    if (buffer == InvalidBuffer)
    {
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }
    else if (blkno != BufferGetBlockNumber(buffer))
    {
      UnlockReleaseBuffer(buffer);
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }
    /* else new pointer points to the same page, no work needed */

    page = BufferGetPage(buffer);

    isnull = SpGistPageStoresNulls(page) ? true : false;
    


    if (SpGistPageIsLeaf(page))
    {
      // pgcnt++;
      SpGistLeafTuple leafTuple;
      OffsetNumber max = PageGetMaxOffsetNumber(page);
      // Datum   leafValue = (Datum) 0;
      // bool    recheck = false;

      //debug
      int cnt = 0;
      //debug
      if (SpGistBlockIsRoot(blkno))
      {
        cnt = 0;

        /* When root is a leaf, examine all its tuples */
        for (offset = FirstOffsetNumber; offset <= max; offset++)
        {
          leafTuple = (SpGistLeafTuple)
            PageGetItem(page, PageGetItemId(page, offset));
          if (leafTuple->tupstate != SPGIST_LIVE)
          {
            /* all tuples on root should be live */
            elog(ERROR, "unexpected SPGiST tuple state: %d",
               leafTuple->tupstate);
          }

          Assert(ItemPointerIsValid(&leafTuple->heapPtr));
          cnt++;
          // TODO get the BoundingBox for this leaf page and assign it to the opaque 
        }
      }
      else
      {
        /* Normal case: just examine the chain we arrived at */
        while (offset != InvalidOffsetNumber)
        {
          Assert(offset >= FirstOffsetNumber && offset <= max);
          
          leafTuple = (SpGistLeafTuple)
            PageGetItem(page, PageGetItemId(page, offset));
          
          if (leafTuple->tupstate != SPGIST_LIVE)
          {
            if (leafTuple->tupstate == SPGIST_REDIRECT)
            {
              /* redirection tuple should be first in chain */
              Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
              /* transfer attention to redirect point */
              stackEntry->ptr = ((SpGistDeadTuple) leafTuple)->pointer;
              Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
              goto redirect;
            }
            if (leafTuple->tupstate == SPGIST_DEAD)
            {
              /* dead tuple should be first in chain */
              Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
              /* No live entries on this page */
              Assert(leafTuple->nextOffset == InvalidOffsetNumber);
              break;
            }
            /* We should not arrive at a placeholder */
            elog(ERROR, "unexpected SPGiST tuple state: %d",
               leafTuple->tupstate);
          }

          Assert(ItemPointerIsValid(&leafTuple->heapPtr));
          
          //TODO: get the Bounding Box for all the points that are in this page leaf
          // Point *p = DatumGetPointP(PointerGetDatum(SGLTDATAPTR(leafTuple)));
          
          cnt++;

          // printf("%d,%f,%f\n" ,stackEntry->level, 
          //     p->x , p->y );


          offset = leafTuple->nextOffset;
        }
      }
      // printf("Leaf max = %d  --  cnt = %d\n", max , cnt);
      printf("level = %d  ,  Leaf max = %d  ,  cnt = %d  ,  blkno = %d\n" ,stackEntry->level, max , cnt, blkno);
      pgcnt++;
    }
    else  /* page is inner */
    {
      SpGistInnerTuple innerTuple;
      SpGistNodeTuple node;
      int     i;
      
      innerTuple = (SpGistInnerTuple) PageGetItem(page,
                        PageGetItemId(page, offset));

      if (innerTuple->tupstate != SPGIST_LIVE)
      {
        if (innerTuple->tupstate == SPGIST_REDIRECT)
        {
          /* transfer attention to redirect point */
          stackEntry->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
          Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
          goto redirect;
        }
        elog(ERROR, "unexpected SPGiST tuple state: %d",
           innerTuple->tupstate);
      }

      // Point * p = DatumGetPointP(SGITDATAPTR(innerTuple));
      // printf("%d,%f,%f\n" ,stackEntry->level, 
      //         p->x , p->y );
      
      //DEBUG
      // Buffer mybuffer = buffer;
      BlockNumber myblkno;
      OffsetNumber myoffset;
      Page mypage;
      //DEBUG
      SGITITERATE(innerTuple, i, node)
      {
        
        if (ItemPointerIsValid(&node->t_tid))
        {
          ScanStackEntry *newEntry;

          /* Create new work item for this node */
          newEntry = palloc(sizeof(ScanStackEntry));
          newEntry->ptr = node->t_tid;
          newEntry->level = stackEntry->level+1;

          scanStack = lcons(newEntry, scanStack);

          //DEBUG
          myblkno = ItemPointerGetBlockNumber(&node->t_tid);
          myoffset = ItemPointerGetOffsetNumber(&node->t_tid);

          if (buffer == InvalidBuffer)
          {
            buffer = ReadBuffer(index, myblkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
          }
          else if (myblkno != BufferGetBlockNumber(buffer))
          {
            UnlockReleaseBuffer(buffer);
            buffer = ReadBuffer(index, myblkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
          }

          mypage = BufferGetPage(buffer);

          if (SpGistPageIsLeaf(mypage))
          {
            
          }
          else /* Inner Tuple*/
          {
            SpGistInnerTuple myinnerTuple;
            // SpGistNodeTuple mynode;
            
            myinnerTuple = (SpGistInnerTuple) PageGetItem(mypage,
                              PageGetItemId(mypage, myoffset));
            // Point * pp = DatumGetPointP(SGITDATAPTR(myinnerTuple));
            // int w = getQuadrant(p,pp);
            // if(w != i+1)
            //   printf("node[%d] - quad %d - center ( %f , %f )\n", i, w , pp->x , pp->y);
          }
          //DEBUG
        }
      }
      
    }

    /* done with this scan stack entry */
    pfree(stackEntry);
    
  }

  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);

  printf("Page count = %d \n", pgcnt);
  printf("\n------------------------------------Finish\n");
}




/*  static bool
my_computeDistance(IndexScanDesc scan, SPGISTSearchItem * item, int which, bool isLeaf , Point * leafVal)
*/

Datum 
myspgist_point_distance(PG_FUNCTION_ARGS)
{

  IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
  my_SpGistScanOpaque so = (my_SpGistScanOpaque) scan->opaque;
 
  ScanKey   key = scan->orderByData;
  int     keySize = scan->numberOfOrderBys;
  double     *distance_p = so->distances;
  bool isNull = false;

  SPGISTSearchItem * item = (SPGISTSearchItem *) PG_GETARG_POINTER(1);
  item->data.heap.recheck = false;
  item->data.heap.recheckDistances = false;
  

  int which =  PG_GETARG_UINT16(2);
  bool isLeaf =  PG_GETARG_BOOL(3);
  Point * leafVal = PG_GETARG_POINT_P(4);
  while (keySize > 0)
  {
    bool recheck = false;

    if ((key->sk_flags & SK_ISNULL) || isNull)
    {
      /* Assume distance computes as null and sorts to the end */
      *distance_p = get_float8_infinity();
    }
    else
    {
      double   dist;
      Point *queryPoint = DatumGetPointP(key->sk_argument);
      BOX box;
      
      if(isLeaf) 
        {
          box.low = box.high = *(leafVal);
        }
      else
      {
        recheck = true;
        
        switch(which)
        {
          case 1:
            box.low.x = item->P_center.x;
            box.low.y = item->P_center.y;
            box.high.x = item->P_max.x;
            box.high.y = item->P_max.y;
          break;
          case 2:
            box.low.x = item->P_center.x;
            box.low.y = item->P_min.y;
            box.high.x = item->P_max.x;
            box.high.y = item->P_center.y;
          break;
          case 3:
            box.low.x = item->P_min.x;
            box.low.y = item->P_min.y;
            box.high.x = item->P_center.x;
            box.high.y = item->P_center.y;
          break;
          case 4:
            box.low.x = item->P_min.x;
            box.low.y = item->P_center.y;
            box.high.x = item->P_center.x;
            box.high.y = item->P_max.y;
          break;
          default:
          break;
        }
      }
      
      // elog(NOTICE, "my_computeDistance ----------- 1");
      dist = computeDistance( isLeaf,  &box, queryPoint);
      // elog(NOTICE, "my_computeDistance ----------- 2");

      item->data.heap.recheckDistances |= recheck;
      *distance_p = dist;
    }

    key++;
    distance_p++;
    keySize--;
  }

  // return true;
  PG_RETURN_BOOL(true);

}



void
myspgcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
        Cost *indexStartupCost, Cost *indexTotalCost,
        Selectivity *indexSelectivity, double *indexCorrelation)
{
  IndexOptInfo *index = path->indexinfo;
  List     *qinfos;
  GenericCosts costs;
  Cost    descentCost;

  // printf("\n------------------------------------myspgcostestimate:\n");
  // printf("==== path:\n");
  // pprint(path);

  /* Do preliminary analysis of indexquals */
  qinfos = deconstruct_indexquals(path);

  MemSet(&costs, 0, sizeof(costs));

  genericcostestimate(root, path, loop_count, qinfos, &costs);

  // DEBUG START
  // printf("==== path After:\n");
  // pprint(path);

  // printf("==== root After:\n");
  // pprint(root);

  // printf("==== costs:\n");
  // printf("indexStartupCost = %f\n" , costs.indexStartupCost);
  // printf("indexTotalCost = %f\n" , costs.indexTotalCost);
  // printf("indexSelectivity = %f\n" , costs.indexSelectivity);
  // printf("indexCorrelation = %f\n" , costs.indexCorrelation);
  // printf("numIndexPages = %f\n" , costs.numIndexPages);
  // printf("numIndexTuples = %f\n" , costs.numIndexTuples);
  // printf("spc_random_page_cost = %f\n" , costs.spc_random_page_cost);
  // printf("num_sa_scans = %f\n" , costs.num_sa_scans);
  

  // DEBUG END
  

  /* ==========================================================
   * =========         TO DO                    ===============
   * ==========================================================
   * K = root->limit_tuples 
   * path->indexorderbys != NIL ( means ORDER BY operator can retreive the query point) 
   * get the blkno of the query point (????)
   * get the cost from the catalog
   ========================================================== */
  // printf("==== path:\n");
  // pprint(path);

  Point * queryPoint = NULL;
  int K = 0;
  int blkno = 0;
  int cost = 1;
  
  /* get the query point */
  List     *indexOrderBys = path->indexorderbys;
  if(indexOrderBys != NIL) // ORDER BY operator
  {
    ListCell   *lc;
    foreach(lc, indexOrderBys)
    {
      OpExpr* opr = (OpExpr*) lfirst(lc);
      if( IsA(opr,OpExpr)  &&  opr->opno == 517  && list_length(opr->args) == 2 )
      {
        Var      *leftvar;
        // Var      *rightvar;
        Const    *rightvar;
        leftvar = (Var *) get_leftop((Expr *) opr);
        rightvar = (Const *) get_rightop((Expr *) opr);

        if( IsA(rightvar , Const) && rightvar->consttype == 600 && !rightvar->constisnull)
        {
           queryPoint = DatumGetPointP(rightvar->constvalue);
        }

      } 
    }
  }

  // printf("==== path:\n");
  // pprint(path);

  /* get the K value */
  if(queryPoint != NULL) // if not null then there is an Order By operator with Point type
  {
    K = (root->limit_tuples > 0)? root->limit_tuples : root->tuple_fraction ;
    /* get the blkno that this query point is located in */
    // char *indexName = malloc (sizeof(indexName) * NAMEDATALEN);
    
    // blkno = myspgWalk(path->indexinfo->indexoid , queryPoint , indexName);
     blkno =  ReadGrid(path->indexinfo->indexoid , queryPoint);

    // blkno = 25;
    elog(NOTICE, "spgCost ================  ");
    // elog(NOTICE , "blkno = %d - K = %d -  q (%f, %f )\n", blkno, K , queryPoint->x , queryPoint->y);
    // // elog(NOTICE , "Block_arr[%d]  ,center(%f,%f)  min (%f,%f) max (%f,%f) \n", blkno , 
    //                        Block_arr[blkno]->P_center.x , Block_arr[blkno]->P_center.y ,
    //                        Block_arr[blkno]->P_min.x , Block_arr[blkno]->P_min.y ,
    //                        Block_arr[blkno]->P_max.x , Block_arr[blkno]->P_max.y);
  
  
    
    /* get the cost from the catalog */
    // cost = FindCost_catalog(blkno , K);
    // cost = FindCost_catalogTbl( get_rel_name(path->indexinfo->indexoid), blkno, K);
    // cost = FindCost_catalogTbl_Bin(indexName, blkno, K);
    cost = FindCost_catalogTbl_Bin(path->indexinfo->indexoid, blkno, K);
    elog(NOTICE, "[%d] -> [%d, %d]\n", blkno , K , cost);

    // elog(NOTICE , "Catalog of Block = %d", blkno);
    // print_catalog(&Block_arr[blkno]->catalog_center);  

    ((Path*)path)->type =  T_IndexPath;
    ((Path*)path)->pathtype = T_IndexScan;
    // printf("==== path:\n");
    // pprint(path);

  }

  
  /* ----- Update the cost ------- */
  double num_scans = costs.num_sa_scans * loop_count;
  double myNumIndexPagesScanned = cost;
  if(num_scans > 1)
  {
    double    pages_fetched;
    IndexOptInfo *index = path->indexinfo;
    /* total page fetches ignoring cache effects */
    pages_fetched = costs.numIndexPages * num_scans;

    /* use Mackert and Lohman formula to adjust for cache effects */
    pages_fetched = index_pages_fetched(pages_fetched,
                      index->pages,
                      (double) index->pages,
                      root);

    costs.indexTotalCost -= (pages_fetched * costs.spc_random_page_cost)
      / loop_count;

    pages_fetched = myNumIndexPagesScanned * num_scans;

    /* use Mackert and Lohman formula to adjust for cache effects */
    pages_fetched = index_pages_fetched(pages_fetched,
                      index->pages,
                      (double) index->pages,
                      root);
    costs.indexTotalCost += (pages_fetched * costs.spc_random_page_cost)
      / loop_count;
  }
  else
  {
     costs.indexTotalCost -= costs.numIndexPages * costs.spc_random_page_cost;
    costs.indexTotalCost += myNumIndexPagesScanned * costs.spc_random_page_cost;
  }

  costs.numIndexPages = myNumIndexPagesScanned;
  int numTuplesPerPage = 1000; 
  costs.numIndexTuples = myNumIndexPagesScanned * numTuplesPerPage;
  path->path.rows = costs.numIndexTuples;
  
  // printf("==== costs  AFTER  :\n");
  // printf("indexStartupCost = %f\n" , costs.indexStartupCost);
  // printf("indexTotalCost = %f\n" , costs.indexTotalCost);
  // printf("indexSelectivity = %f\n" , costs.indexSelectivity);
  // printf("indexCorrelation = %f\n" , costs.indexCorrelation);
  // printf("numIndexPages = %f\n" , costs.numIndexPages);
  // printf("numIndexTuples = %f\n" , costs.numIndexTuples);
  // printf("spc_random_page_cost = %f\n" , costs.spc_random_page_cost);
  // printf("num_sa_scans = %f\n" , costs.num_sa_scans);
 

  /*
   * We model index descent costs similarly to those for btree, but to do
   * that we first need an idea of the tree height.  We somewhat arbitrarily
   * assume that the fanout is 100, meaning the tree height is at most
   * log100(index->pages).
   *
   * Although this computation isn't really expensive enough to require
   * caching, we might as well use index->tree_height to cache it.
   */
  if (index->tree_height < 0) /* unknown? */
  {
    if (index->pages > 1) /* avoid computing log(0) */
      index->tree_height = (int) (log(index->pages) / log(100.0));
    else
      index->tree_height = 0;
  }

  /*
   * Add a CPU-cost component to represent the costs of initial descent. We
   * just use log(N) here not log2(N) since the branching factor isn't
   * necessarily two anyway.  As for btree, charge once per SA scan.
   */
  if (index->tuples > 1)    /* avoid computing log(0) */
  {
    descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
    costs.indexStartupCost += descentCost;
    costs.indexTotalCost += costs.num_sa_scans * descentCost;
  }

  /*
   * Likewise add a per-page charge, calculated the same as for btrees.
   */
  descentCost = (index->tree_height + 1) * 50.0 * cpu_operator_cost;
  costs.indexStartupCost += descentCost;
  costs.indexTotalCost += costs.num_sa_scans * descentCost;

  *indexStartupCost = costs.indexStartupCost;
  *indexTotalCost = costs.indexTotalCost;
  *indexSelectivity = costs.indexSelectivity;
  *indexCorrelation = costs.indexCorrelation;
}



//=================================================
//========= Catalog builder fn Version 2 ==========
//=================================================
#include "catalog/heap.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "catalog/index.h"
#include "catalog/toasting.h"
#include "catalog/pg_tablespace.h"
#include "access/xact.h"

#include "executor/spi.h"
void createCatalogRelations(char * indexName);
void createCatalogRelations_Bin(Oid indexoid);
void writeToFile(void);
void ReadFromFile(void);
void writeToFile2(void);
void ReadFromFile2(void);
void writeGrid(char* GridName);


void fillGrid(int * grid ,Point Pmin , double w, double h, int nrows , int ncols, Relation index, SpGistState *state);
void createGrid( Relation index, SpGistState *state );
double my_round(double x, unsigned int digits);
void ReadGrid2(Oid indexid );

Datum
build_catalog2(PG_FUNCTION_ARGS)
{
  elog(NOTICE, "build_catalog2 ------------ start\n");

  #if PG_VERSION_NUM < 90200
  elog(NOTICE, "Function is not working under PgSQL < 9.2");

  PG_RETURN_TEXT_P(CStringGetTextDatum("???"));
  #else

  //*****************************
  //*****************************
  // writeGrid("customer_location_spgist_quad2_index");
  // int blkno = ReadGrid("customer_location_spgist_quad2_index");

  // elog(NOTICE, "Grid ..... Blkno = %d", blkno);
  // ReadGrid2(114929);
  // PG_RETURN_INT32(1000);
  //*****************************
  //*****************************
  //==============================  Open Index table  ======================================================

  text      *name=PG_GETARG_TEXT_P(0);
  RangeVar    *relvar;
  Relation    index;
  // ItemPointerData ipd;
  
  relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
  index = relation_openrv(relvar, AccessExclusiveLock);

  // pprint(index);

  if (!IS_INDEX(index) || (!IS_SPGIST(index) && !IS_SPGIST2(index)))
    elog(ERROR, "relation \"%s\" is not an SPGiST index",
       RelationGetRelationName(index));
  //elog(NOTICE, "free (name)");
  pfree(name);
  
  /* -------------------------------------  
   * Fill an array of datablocks (pages)  
   * ------------------------------------- */
   SpGistState state;
   initSpGistState(&state, index);
  

   ReadDataBlocks( index , &state);
  

  /* -------------------------------------  
   * Build the catalog 
   *     1 . For each block, fill the BlockQ with minDist 
             1. Fill the tupleQ 
             2. complete the catalog. table for this table  
   * ------------------------------------- */
  
  BuildCatalogLogic(&state , index);

  /* -------------------------------------
   *  Create tables for the catalogs
   * ------------------------------------- */ 

  // createCatalogRelations(RelationGetRelationName(index));
  createCatalogRelations_Bin(index->rd_id);
  createGrid( index, &state );
  

  index_close(index, AccessExclusiveLock);
  PG_RETURN_INT32(1000);
    #endif
}

//--------------------------------
//--------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>


#include <sys/mman.h>

//MAC
#define FILEPATH "/Users/princess/Documents/Masters/code/kNNContribution/outputs/knn_hook.bin"
#define filepath  "/Users/princess/Documents/Masters/code/kNNContribution/outputs/"
//Server
//#define filepath  "/home/samy/Amira/pgsql2/data/KNNcatalog2/"


#define NUMINTS  (10)
#define FILESIZE (NUMINTS * sizeof(int))
#define GRID_PARAMS 8
void createCatalogRelations(char * indexName)
{
  char * catalogName = malloc(strlen(indexName) + 10 + sizeof(BlockNumber));
  char * SQL_CREATE = NULL;// = malloc(sizeof(SQL_CREATE) * 100 + sizeof(catalogName));
  char * SQL_INSERT = NULL;
  char * values = NULL;

  int res = 0;
  //DEBUG
  // BlockNumber num = 321;
  // sprintf(catalogName , "%s_CENTER_%d",indexName , num ) ; 
  // sprintf(catalogName , "%s-XXXXXX" , catalogName);
  // elog(NOTICE, "Create Catalog Relations:  catalog Name = %s", catalogName);
  // return;
  int k = 0;
  //DEBUG
  
  /* connect */
  if (SPI_connect() != SPI_OK_CONNECT)
      elog(ERROR, "SPI_connect failed");


  BlockNumber blkno ;
  for (blkno = 0; blkno < MAX_NO_LEAF_PAGE; blkno++)
    {
      if(Block_arr[blkno] == NULL)
        continue;

      /* update the relation name */
      catalogName = NULL;
      catalogName = malloc(strlen(indexName) + 10 + sizeof(BlockNumber));
      sprintf(catalogName , "%s_CENTER_%d",indexName , blkno ) ; 

      /* if table exists delete it before creating it again */
      char * SQL_DELETE = NULL;
      SQL_DELETE = malloc(sizeof(SQL_DELETE) * 100 + strlen(catalogName));
      sprintf(SQL_DELETE , "DROP TABLE %s ;" , catalogName);

      res = SPI_exec(SQL_DELETE  , 0);
    

      /* update sql query */
      SQL_CREATE = NULL;
      SQL_CREATE = malloc(sizeof(SQL_CREATE) * 100 + strlen(catalogName));
      sprintf(SQL_CREATE , "CREATE TABLE %s (K int PRIMARY KEY, Cost int);" , catalogName);

      res = SPI_exec(SQL_CREATE  , 0);
    
      if(res > 0) // succeed in creation 
      {
        /* insert values in the table */
        
        values = NULL;
        values = malloc(sizeof(values) * (sizeof(int)*2) * Block_arr[blkno]->catalog_center.size);
        sprintf(values , "( ");

        //DEBUG
        elog(NOTICE, "Blkno = %d Catalog_center : ", blkno);
        //DEBUG

        int iter = 0;
        for (; iter < Block_arr[blkno]->catalog_center.size; iter++)
        {
          sprintf(values , "%s%d,%d)" , values , Block_arr[blkno]->catalog_center.key[iter],
                                                Block_arr[blkno]->catalog_center.cost[iter]);
          if(iter+1 < Block_arr[blkno]->catalog_center.size)
            sprintf(values, "%s,(" , values);

          //DEBUG
          // elog(NOTICE , "%d | %d", Block_arr[blkno]->catalog_center.key[iter] , 
          //                          Block_arr[blkno]->catalog_center.cost[iter]);
          //DEBUG
        }
        
        //DEBUG
        // elog(NOTICE, "values = %s" , values);
        //DEBUG
        SQL_INSERT = NULL;
        SQL_INSERT = malloc(sizeof(SQL_INSERT) * 100 + strlen(catalogName) + strlen(values) );
        
        sprintf(SQL_INSERT , "INSERT INTO %s (K , Cost) VALUES %s ;",
                              catalogName , values);
        res = SPI_exec(SQL_INSERT, 0);
        if(res < 0)
          elog(ERROR, "failed in Insertion SQL Query = %s", SQL_INSERT);
      }
      else
        elog(ERROR , "failed in Execute SQL query %s" , SQL_CREATE);
      //DEBUG
      // k++;
      // if(k == 1)
      //   break;
      //DEBUg
    }

  /* close connection */
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "SPI_finish failed");      
}

// void createCatalogRelations_Bin(char * indexName)
void createCatalogRelations_Bin(Oid indexoid)
{
  elog(NOTICE, "createCatalogRelations_Bin -------- start");

  // char * path = malloc(sizeof(path) * 200);
  // path = "/Users/princess/Documents/Masters/code/kNNContribution/outputs/";

  char * catalogName = malloc(strlen(filepath)+sizeof(indexoid) + 10 + sizeof(BlockNumber));
  
  int fd;
  int result;

  BlockNumber blkno ;
  for (blkno = 0; blkno < MAX_NO_LEAF_PAGE; blkno++)
    {
      if(Block_arr[blkno] == NULL)
        continue;

      /* update the relation name */
      catalogName = NULL;
      catalogName = malloc(strlen(filepath)+sizeof(indexoid) + 10 + sizeof(BlockNumber));
      sprintf(catalogName , "%s%d%dC.bin",filepath, indexoid , blkno ) ; 

      
    int size = Block_arr[blkno]->catalog_center.size;
    int filesize = sizeof(int) * size * 2 + 2;
    /* Open a file for writing.
     *  - Creating the file if it doesn't exist.
     *  - Truncating it to 0 size if it already exists. (not really needed)
     *
     * Note: "O_WRONLY" mode is not sufficient when mmaping.
     */
    fd = open(catalogName, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (fd == -1) {
      elog(ERROR,"1: Error opening file for writing");
    }
    
    /* Stretch the file size to the size of the (mmapped) array of ints
     */
    result = lseek(fd, filesize-1, SEEK_SET);
    if (result == -1) {
      close(fd);
      elog(ERROR,"1: Error calling lseek() to 'stretch' the file");
    }
    /* Something needs to be written at the end of the file to
     * have the file actually have the new size.
     * Just writing an empty string at the current file position will do.
     *
     * Note:
     *  - The current position in the file is at the end of the stretched 
     *    file due to the call to lseek().
     *  - An empty string is actually a single '\0' character, so a zero-byte
     *    will be written at the last byte of the file.
     */
    result = write(fd, "", 1);
    if (result != 1) {
      close(fd);
      elog(ERROR, "1: Error writing last byte of the file");
    }

    /* Now the file is ready to be mmapped.
     */
    int (*map) =  (int (*)) mmap(0, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
    close(fd);
    elog(ERROR, "1: Error mmapping the file");
     // exit(EXIT_FAILURE);
    }
    
    /* Now write int's to the file as if it were memory (an array of ints).
     */
    // write the size of the table in the first line
    map[1] = size;
    int i;
    int j = 2;
    for (i = 0; i <size; ++i) {
      map[j++] = Block_arr[blkno]->catalog_center.key[i];
      map[j++] = Block_arr[blkno]->catalog_center.cost[i];

      }

    /* Don't forget to free the mmapped memory
     */
    if (munmap(map, filesize) == -1) {
      elog(ERROR, "1: Error un-mmapping the file");
      /* Decide here whether to close(fd) and exit() or not. Depends... */
        }
    
    /* Un-mmaping doesn't close the file, so we still need to do that.
     */
    close(fd);
    
        
        
    }  
    elog(NOTICE, "createCatalogRelations_Bin -------- FINISH\n\n");   
}

void writeGrid(char * gridName)
{
  elog(NOTICE, "createGrid -------- start");
  char * FullGridName = malloc(strlen(filepath)+strlen(gridName) + 10 );
  sprintf(FullGridName , "%s%s.bin",filepath, gridName ) ; 

  elog(NOTICE, "createGrid -------- 1  GridName %s", FullGridName);
  int fd;
  int result;

  // int ** Grid;
  int r = 6;
  int c = 5;
  int i;
  int **Grid = (int **)malloc(r * sizeof(int *));
  for (i=0; i<r; i++)
       Grid[i] = (int *)malloc(c * sizeof(int));
  
  elog(NOTICE, "createGrid -------- 2");
  int filesize = sizeof(int) * r * c + 10;
    /* Open a file for writing.
     *  - Creating the file if it doesn't exist.
     *  - Truncating it to 0 size if it already exists. (not really needed)
     *
     * Note: "O_WRONLY" mode is not sufficient when mmaping.
     */
  fd = open(FullGridName, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
  if (fd == -1) {
    elog(ERROR,"1: Error opening file for writing");
    }
  elog(NOTICE, "createGrid -------- 3"); 
    /* Stretch the file size to the size of the (mmapped) array of ints
     */
    result = lseek(fd, filesize-1, SEEK_SET);
    if (result == -1) {
      close(fd);
      elog(ERROR,"1: Error calling lseek() to 'stretch' the file");
    }
    /* Something needs to be written at the end of the file to
     * have the file actually have the new size.
     * Just writing an empty string at the current file position will do.
     *
     * Note:
     *  - The current position in the file is at the end of the stretched 
     *    file due to the call to lseek().
     *  - An empty string is actually a single '\0' character, so a zero-byte
     *    will be written at the last byte of the file.
     */
    elog(NOTICE, "createGrid -------- 4");
    result = write(fd, "", 1);
    if (result != 1) {
      close(fd);
      elog(ERROR, "1: Error writing last byte of the file");
    }

    /* Now the file is ready to be mmapped.
     */

    elog(NOTICE, "createGrid -------- 5");


    int (*map) =  (int *) mmap(0, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
    close(fd);
    elog(ERROR, "1: Error mmapping the file");
     // exit(EXIT_FAILURE);
    }
    
    /* Now write int's to the file as if it were memory (an array of ints).
     */
    // write the size of the table in the first line
    
    int j ;
    int cnt = 1;
    for (i = 0; i <r; ++i) 
    {
     for (j = 0; j <c; ++j) 
     {
      // map[i][j] = cnt++;
      map[i * (r) + j] = cnt++;  // setting the grid[4][5]

      }
    }

   

    /* Don't forget to free the mmapped memory
     */
    if (munmap(map, filesize) == -1) {
      elog(ERROR, "1: Error un-mmapping the file");
      /* Decide here whether to close(fd) and exit() or not. Depends... */
        }
    
    /* Un-mmaping doesn't close the file, so we still need to do that.
     */
    close(fd);
    
        
        
      
    elog(NOTICE, "createGrid -------- FINISH\n\n");   
}

double my_round(double x, unsigned int digits) {
    double fac = pow(10, digits);
    return round(x*fac)/fac;
}

void GridParams(Oid indexid , double* p_minW , double* p_minH ,
                              int * nrows , int *ncols )
{
  char * FullGridName = malloc(strlen(filepath)+sizeof(indexid) + 10 );
  sprintf(FullGridName , "%s%d.bin",filepath, indexid ) ; 

  elog(NOTICE, "GridParams -------- 1  GridName %s", FullGridName);
  elog(NOTICE, "GridParams -------- 1  minP (%f,%f) - maxP (%f,%f)", 
                                    minP.x , minP.y, maxP.x , maxP.y);

  /* step 1  Preparation */
  BlockNumber blkno = 0;
  double  minW = -1 ,  minH = -1;

  for(; blkno < MAX_NO_LEAF_PAGE; blkno++)
  {
    if(Block_arr[blkno] == NULL)
        continue;

    double w =  Block_arr[blkno]->P_max.x - Block_arr[blkno]->P_min.x;
    double h = Block_arr[blkno]->P_max.y - Block_arr[blkno]->P_min.y;
    
    if(minW == -1) // first time
      minW = w;
    else if(minW > w)
      minW = w;
    
    if(minH == -1) // first time
      minH = h;
    else if(minH > h)
      minH = h;
  }
  *p_minW = my_round(minW , 2);
  *p_minH = my_round(minH , 2);

  if(*p_minW == 0) 
    *p_minW = 0.1; // min TODO 
  if(*p_minH == 0)
    *p_minH = 0.1; // TODO: make global min Val

  elog(NOTICE, "GridParams -------- 2  minW = %f, minH = %f , p_minW = %f, p_minH = %f",
                                       minW , minH , *p_minW , *p_minH);

  /* initialize the size */
  *nrows = ceil(maxP.y/minH);
  *ncols = ceil(maxP.x/minW);
  
}

void createGrid( Relation index, SpGistState *state )
{
  elog(NOTICE, "createGrid -------- start");
  
  //=======================
  /* File Name */
  Oid indexid = index->rd_id;
  char * FullGridName = malloc(strlen(filepath)+sizeof(indexid) + 10 );
  sprintf(FullGridName , "%s%d.bin",filepath, indexid ) ; 
  elog(NOTICE, "createGrid -------- 1  GridName %s", FullGridName);

  //=======================
  /* Grid Params */
  double  minW = -1 ,  minH = -1;
  int r , c;

  GridParams(indexid , &minW , &minH , &r , &c );
  elog(NOTICE, "createGrid -------- 2  minW = %f, minH = %f , r = %d , c = %d", 
                                       minW , minH , r , c);
  //=======================
  /* Create File */
  int fd;
  int result;
  int filesize;
  unsigned char * fullMap;


  elog(NOTICE, "createGrid -------- 2.1");
  
  /* initialize the File size */
  filesize = sizeof(int) * r * c  + sizeof(double) * GRID_PARAMS;
  
  fd = open(FullGridName, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
  if (fd == -1) {
    elog(ERROR,"1: Error opening file for writing");
    }
  
  elog(NOTICE, "createGrid -------- 3"); 
  /* Stretch the file size to the size of the (mmapped) array of ints */
  result = lseek(fd, filesize-1, SEEK_SET);
  if (result == -1) {
    close(fd);
    elog(ERROR,"1: Error calling lseek() to 'stretch' the file with errorno = %d", errno);
  }
 
  elog(NOTICE, "createGrid -------- 4");
  result = write(fd, "", 1);
  if (result != 1) {
    close(fd);
    elog(ERROR, "1: Error writing last byte of the file");
  }

  elog(NOTICE, "createGrid -------- 5");

  //=======================
  /* map the full data */
  fullMap =  (unsigned char *) mmap(0, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (fullMap == MAP_FAILED) {
  close(fd);
  elog(ERROR, "1: Error mmapping the file for Grid Params");
  }

  //=======================
  /* save Grid Params :  the size , minW , minH , minP  */
  int gridParamSize = sizeof(double) * GRID_PARAMS;
  double * grid = (double *)fullMap;
  
  grid[0] = (double)r;
  grid[1] = (double)c;
  grid[2] = minW;
  grid[3] = minH;
  grid[4] = minP.x;
  grid[5] = minP.y;
  grid[6] = maxP.x;
  grid[7] = maxP.y;
  
  // if (munmap(grid, filesize) == -1) {
  //   elog(ERROR, "1: Error un-mmapping the file for Grid Params");
  //   /* Decide here whether to close(fd) and exit() or not. Depends... */
  // }
  elog(NOTICE, "createGrid -------- 6");

  //=======================
  /* fill the Grid */
  int gridDataSize = sizeof(int) * r * c  ;
  size_t offset = sizeof(double) * GRID_PARAMS; 
  
  
  elog(NOTICE, "createGrid -------- 7 , offset = %zu , filesize= %d " , offset , filesize);

  int * map = (int *)(fullMap +offset); 
  
  elog(NOTICE, "createGrid -------- 7 , fullMap = %p , map = %p , grid = %p",
                                      fullMap, map , grid);
  fillGrid(map ,minP ,  minW, minH, r , c, index, state);

  //=======================
  /* unmap the fullMap */
  if (munmap(fullMap, filesize) == -1) {
    elog(ERROR, "1: Error un-mmapping the file");
    /* Decide here whether to close(fd) and exit() or not. Depends... */
  }
  
  /* Un-mmaping doesn't close the file, so we still need to do that.
   */
  close(fd);
       
  elog(NOTICE, "createGrid -------- FINISH\n\n");   
}

void fillGrid(int * grid ,Point Pmin , double w, double h, int nrows , int ncols, Relation index, SpGistState *state)
{
  elog(NOTICE, "fillGrid ----------------- start\n");
  
  elog(NOTICE, "fillGrid ----------------- 1  %f - %f - %d - %d \n", 
                                              w , h , nrows , ncols);
  /* Initialize with negative */
  int j , i;
  for (i = 0; i < nrows; ++i) 
  {
   for (j = 0; j <ncols; ++j) 
   {
      // elog(NOTICE, "[%d][%d]  - [%d]" , i , j , i * (ncols) + j);
      grid[i * (ncols) + j] = -1;  // initializing
      // *(grid + i * (ncols) + j) [i * (nrows) + j] = -1;  // initializing
   }
  }

  //DEBUG
  elog(NOTICE, "fillGrid ----------------- 2 initializing done ");

  int cnt = 1;
  printf("\n\nfillGrid ---------- Fill with tree traversal start\n");
  printf(" Size rows x cols = %d x %d\n\n" , nrows , ncols);
  //DEBUG
  /* traverse the quadtree and fill the grid */
  BlockNumber p_blkno = 0 , dummyNo = SPGIST_ROOT_BLKNO;

  for(; p_blkno < MAX_NO_LEAF_PAGE; p_blkno++)
  {
    if(Block_arr[p_blkno] == NULL)
        continue;

    // get the pointer of this datablock and retrive all its points 
    Buffer buffer = InvalidBuffer;
    BlockNumber blkno;
    OffsetNumber offset, p_offset;
    Page    page;

    // elog(NOTICE, "fillGrid ----------------- 2.1 , blkno = %d" , p_blkno);
    
    for(p_offset = 0 ; p_offset < MAX_NO_LEAF_OFFSETS; p_offset++)
    {
      
      if(Block_arr[p_blkno]->offset[p_offset] == 0)
        continue;
      
      // elog(NOTICE, "fillGrid ----------------- 2.2 , offset = %d" , p_offset);
      // elog(NOTICE, "FillTupleQ ----------------- 1");
      blkno = ItemPointerGetBlockNumber(&Block_arr[p_blkno]->ptr[p_offset]);
      offset = ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]);

      // elog(NOTICE, "FillTupleQ ----------------- 2");
      // insanity check 
      Assert((blkno == p_blkno));
      Assert((offset == p_offset));

      // elog(NOTICE, "FillTupleQ ----------------- 3");
      if (buffer == InvalidBuffer)
      {
        buffer = ReadBuffer(index, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
      }
      else if (blkno != BufferGetBlockNumber(buffer))
      {
        UnlockReleaseBuffer(buffer);
        buffer = ReadBuffer(index, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
      }

      // elog(NOTICE, "FillTupleQ ----------------- 4");
      page = BufferGetPage(buffer);
      
      // elog(NOTICE, "FillTupleQ ----------------- 5");

      if (SpGistPageIsLeaf(page)) 
      {
        SpGistLeafTuple leafTuple;
        OffsetNumber max = PageGetMaxOffsetNumber(page);
        
        
        if (SpGistBlockIsRoot(blkno))
        {
          // elog(NOTICE, "FillTupleQ ----------------- 7");
           //=========================
            // int batchR = fabs(Block_arr[p_blkno]->P_max.y - Block_arr[p_blkno]->P_min.y)/h;
            // int batchC = fabs(Block_arr[p_blkno]->P_max.x - Block_arr[p_blkno]->P_min.x)/w;
            // double stPx = Block_arr[p_blkno]->P_min.x;
            // double stPy = Block_arr[p_blkno]->P_min.y;
            
            // int r =0 , c=0;
            // for(r= fabs((stPy - Pmin.y)/h); r < batchR; r++ )
            // {
            //   for(c= fabs((stPx - Pmin.x)/w); r < batchC; c++ )
            //   {
            //     grid[r * batchC + c] = blkno;
            //   }
            // }
            //=========================
          /* When root is a leaf, examine all its tuples */
          for (offset = FirstOffsetNumber; offset <= max; offset++)
          {
            leafTuple = (SpGistLeafTuple)
              PageGetItem(page, PageGetItemId(page, offset));
            if (leafTuple->tupstate != SPGIST_LIVE)
            {
              /* all tuples on root should be live */
              elog(ERROR, "unexpected SPGiST tuple state: %d",
                 leafTuple->tupstate);
            }

            Assert(ItemPointerIsValid(&leafTuple->heapPtr));

            /* retrieve the point */
            Point *p = DatumGetPointP(SGLTDATUM(leafTuple, state));
            
            /* fill the grid */
            int Gx = fabs((p->x - Pmin.x)/w);
            int Gy = fabs((p->y - Pmin.y)/h);

            grid[Gy * (ncols) + Gx] = blkno;
            dummyNo = blkno;
            // printf("[%d][%d] = %d\n" , Gy , Gx , blkno);
            cnt ++;

          }
        }
        else
        {
          // elog(NOTICE, "FillTupleQ ----------------- 8");
           //=========================
            // int batchR = fabs(Block_arr[p_blkno]->P_max.y - Block_arr[p_blkno]->P_min.y)/h;
            // int batchC = fabs(Block_arr[p_blkno]->P_max.x - Block_arr[p_blkno]->P_min.x)/w;
            // batchR = (batchR > nrows )? nrows : batchR;
            // batchC = (batchC > ncols )? ncols : batchC;

            // double stPx = Block_arr[p_blkno]->P_min.x;
            // double stPy = Block_arr[p_blkno]->P_min.y;
            
            // elog(NOTICE, "Pmin(%f,%f) , Pmax(%f,%f) , batchC = %d , batchR = %d , stPx = %f , stPy = %f",
            //               Block_arr[p_blkno]->P_min.x, Block_arr[p_blkno]->P_min.y,
            //               Block_arr[p_blkno]->P_max.x, Block_arr[p_blkno]->P_max.y,
            //               batchC , batchR , stPx , stPy);

            // // elog(NOTICE, "FillTupleQ ----------------- 9 ");
            // int r =0 , c=0;
            // for(r= fabs((stPy - Pmin.y)/h); r < batchR; r++ )
            // {
            //   for(c= fabs((stPx - Pmin.x)/w); c < batchC; c++ )
            //   {
            //     if(r < 0 || c < 0) 
            //       elog(ERROR, "r || c < 0 : %d , %d " , r , c);

            //     // elog(NOTICE , "[%d][%d] - [%d] ", r , c , (r * batchC + c) );
            //     grid[r * batchC + c] = blkno;
            //   }
            // }
            //=========================
          /* Normal case: just examine the chain we arrived at */
          
          while (offset != InvalidOffsetNumber)
          {
            // elog(NOTICE, "FillTupleQ ----------------- 9");
            
            Assert(offset >= FirstOffsetNumber && offset <= max);
            leafTuple = (SpGistLeafTuple)
              PageGetItem(page, PageGetItemId(page, offset));
            
            // elog(NOTICE, "FillTupleQ ----------------- 10");
            if (leafTuple->tupstate != SPGIST_LIVE)
            {
              // DEBUG
              printf("\n\nLeaf tuple is NOT LIVE\n");
              //DEBUG
              if (leafTuple->tupstate == SPGIST_REDIRECT)
              {
                // DEBUG
                printf("Leaf tuple Redirect\n");
                //DEBUG

                /* redirection tuple should be first in chain */
                Assert(offset == ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]));
                /* transfer attention to redirect point */
                Block_arr[p_blkno]->ptr[p_offset] = ((SpGistDeadTuple) leafTuple)->pointer;
                Assert(ItemPointerGetBlockNumber(&Block_arr[p_blkno]->ptr[p_offset]) != SPGIST_METAPAGE_BLKNO);
                // goto redirect;
              }
              if (leafTuple->tupstate == SPGIST_DEAD)
              {
                // DEBUG
                printf("Leaf tuple is DEAD\n");
                //DEBUG

                /* dead tuple should be first in chain */
                Assert(offset == ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]));
                /* No live entries on this page */
                Assert(leafTuple->nextOffset == InvalidOffsetNumber);
                break;
              }
              /* We should not arrive at a placeholder */
              elog(ERROR, "unexpected SPGiST tuple state: %d",
                 leafTuple->tupstate);
            }

            // elog(NOTICE, "FillTupleQ ----------------- 11");
            Assert(ItemPointerIsValid(&leafTuple->heapPtr));

            // elog(NOTICE, "FillTupleQ ----------------- 12");
            
            /* retrieve the point */
            Point *p = DatumGetPointP(SGLTDATUM(leafTuple, state));
            
            
            offset = leafTuple->nextOffset;

            /* fill the grid */
            int Gx = fabs((p->x - Pmin.x)/w);
            int Gy = fabs((p->y - Pmin.y)/h);

            grid[Gy * (ncols) + Gx] = blkno;
            dummyNo = blkno;
            // printf("[%d][%d] = %d\n" , Gy , Gx , blkno);
            cnt++;

            
          }

          // the Hack 
          //=========================
          // int batchR = fabs(maxY - minY)/h;
          // int batchC = fabs(maxX - minX)/w;
          // batchR = (batchR > nrows )? nrows : batchR;
          // batchC = (batchC > ncols )? ncols : batchC;

          // double stPx = minX;
          // double stPy = minY;
          
          // elog(NOTICE, "Pmin(%f,%f) , Pmax(%f,%f) , batchC = %d , batchR = %d , stPx = %f , stPy = %f , r = %d , c = %d",
          //               minX, minY, maxX, maxY,
          //               batchC , batchR , stPx , stPy, 
          //               ceil(fabs((stPy - Pmin.y)/h)), 
          //               ceil(fabs((stPx - Pmin.x)/w)) );

          // // elog(NOTICE, "FillTupleQ ----------------- 9 ");
          // int r =0 , c=0;
          // // if(batchR == 0 AND batchC > 0) 
          // // {
          // //   if(grid[r * batchC + c] == -1)
          // //     {
          // // }
          // for(r= ceil(fabs((stPy - Pmin.y)/h)); r < batchR; r++ )
          // {
          //   for(c= ceil(fabs((stPx - Pmin.x)/w)); c < batchC; c++ )
          //   {
          //     if(grid[r * batchC + c] == -1)
          //     {
          //       if(r < 0 || c < 0) 
          //         elog(ERROR, "r || c < 0 : %d , %d " , r , c);

          //       // elog(NOTICE , "[%d][%d] - [%d] ", r , c , (r * batchC + c) );
          //       grid[r * batchC + c] = blkno;
          //     }
          //   }
          // }
          //=========================
        }
      }
      else
      {
        elog(ERROR , "This page should be leaf not Inner ... there is a Problem\n");
      }
    }
    // elog(NOTICE, "FillTupleQ ----------------- 17");
    if (buffer != InvalidBuffer)
      UnlockReleaseBuffer(buffer);
  }

  

  elog(NOTICE, "fillGrid ----------------- 3 traverse done - cnt = %d ", cnt);

  //DEBUG
  // for (i = 0; i <nrows; ++i) 
  //   {
  //    for (j = 0; j <ncols; ++j) 
  //    {
  //     printf( "%d ",grid[i * (ncols) + j]);
  //    }
  //    printf( "\n\n");
  //   }
    printf("\n\nfillGrid ---------- Fill with tree traversal Finish \n ");
  //DEBUG

  /* Final step go throw the grid and fill unfilled cells */
  Point p ;
  int blk;
  for (i = 0; i <nrows; i++) 
  {
    // p.y = i + h/2.0;
    p.y = Pmin.y + i*h;
   for (j = 0; j <ncols; ++j) 
   {
      if(grid[i * (ncols) + j] == -1)
      {
        // elog(NOTICE, "fillGrid ----------------- 3.1 , [%d][%d] - Point (%f,%f)" , i,j, p.x , p.y);

        p.x = Pmin.x + j*w;
        blk=0;

        initSpGistState(state, index);
        blk =FindPoint( index , state , &p);//         myspgWalk(index->rd_id, p)//, char * indexName)
        if(blk == SPGIST_ROOT_BLKNO)
          blk = dummyNo;
        // blk = FindEnclosedBlk(&p);
        // elog(NOTICE, "fillGrid ----------------- 3.2 , blkno = %d", blk);
        grid[i * (ncols) + j] = blk;  
      }
    }
  }

  elog(NOTICE, "fillGrid ----------------- END");
}



#define MAX_GRID_COLS 20000
#define MAX_GRID_ROWS 20000
int ReadGrid(Oid indexid , Point* qp)
{
  // elog(NOTICE, "ReadGrid ------ start");
  
  //=================
  /* File Name */
  char * FullGridName = malloc(strlen(filepath)+sizeof(indexid) + 10 );
  sprintf(FullGridName , "%s%d.bin",filepath, indexid ) ; 

  //=================
  /* Open File */
  int fd;
  size_t filesize;
  unsigned char * fullMap;

  fd = open(FullGridName, O_RDONLY);
  if (fd == -1) {
    elog(ERROR, "1: Error opening file for reading");
  }

  // elog(NOTICE, "ReadGrid ------ 1");

  //=================
  /* Map the file */
  filesize = sizeof(int) * MAX_GRID_ROWS * MAX_GRID_COLS + sizeof(double) * GRID_PARAMS ; 

  fullMap = (unsigned char *) mmap(0, filesize, PROT_READ, MAP_SHARED, fd, 0);
  if (fullMap == MAP_FAILED) {
    close(fd);
    elog(ERROR, "1: Error mmapping the file for grid params");
  }
  //=================
  /* Read Grid Params */
  int nrows =0 , ncols= 0;
  double minH , minW ;
  Point minPoint , maxPoint;
  
  int gridParamSize = sizeof(double) * GRID_PARAMS; 

  double *grid = (double *)fullMap;

  nrows = grid[0] ;//= r;
  ncols = grid[1] ;// = c;
  minW = grid[2]  ;//= minW;
  minH = grid[3]  ;//= minH;
  minPoint.x = grid[4];// = minP.x;
  minPoint.y = grid[5];// = minP.y;
  maxPoint.x = grid[6];// = maxP.x;
  maxPoint.y = grid[7];// = maxP.y;

 // if (munmap(grid, filesize) == -1) {
 //    elog(ERROR, "1: Error un-mmapping the file for grid params");
 //  }
  
  // elog(NOTICE, "ReadGrid ------ 2 , Grid Params : %d\n%d\n%f\n%f\n%f\n%f\n%f\n%f",
  //                                          nrows, ncols, minW , minH , 
  //                                          minPoint.x, minPoint.y ,
  //                                          maxPoint.x , maxPoint.y);
  //=================
  /* realloc the map with the new sizes if necessaray */
  if(nrows > MAX_GRID_ROWS || ncols > MAX_GRID_COLS)
  {
    elog(NOTICE, "ReadGrid ----- un-mmapping the file for Remapping ");
    size_t old_size = filesize;
    filesize = sizeof(int) * nrows * ncols + sizeof(double) * GRID_PARAMS ; 
    
    if (munmap(fullMap, old_size) == -1) {
      elog(ERROR, "1: Error un-mmapping the file for Remapping ");
    }
    // fullMap = (unsigned char *)mremap (0, old_size, filesize);
    fullMap = (unsigned char *) mmap(0, filesize, PROT_READ, MAP_SHARED, fd, 0);
  }

  // elog(NOTICE, "ReadGrid ------ 3 ");
  //=================
  /* Find the blkno */
  
  int *map;  /* mmapped array of int's */
  int blkno;
  size_t offset = sizeof(double) * GRID_PARAMS;
  
  // filesize = sizeof(int) * nrows * ncols + sizeof(double) * GRID_PARAMS; 
  // elog(NOTICE, "ReadGrid ------ 4 ");
  map = (int *)(fullMap + offset);
  // if (map == MAP_FAILED) {
  //   close(fd);
  //   elog(ERROR, "1: Error mmapping the file");
  // }
  // elog(NOTICE, "ReadGrid ------ 5 , queryPoint = (%f,%f)" , qp->x ,qp->y);

  double x = fabs( (qp->x - minPoint.x) / minW );
  double y = fabs( (qp->y - minPoint.y) / minH) ;
  
  // elog(NOTICE, "ReadGrid ------ 6 , [%f][%f]" , x , y);

  int Gx = round(x);
  int Gy = round(y);
  // elog(NOTICE, "ReadGrid ------ 6 , [%d][%d]" , Gx , Gy);
  blkno = map[Gy * ncols + Gx];

  // el/                              Gx , Gy , blkno);
    // /* Read the file int-by-int from the mmap
    //  */
    // printf("\n\nGridRead\n");
    // int r = 6, c= 5;
    // int i ;
    // int j ;
    // for (i = 0; i <r; ++i) 
    // {
    //  for (j = 0; j <c; ++j) 
    //  {
    //   printf( "%d ",map[i * (r) + j]);
    //  }
    //  printf( "\n");
    // }
    
  if (munmap(fullMap, filesize) == -1) {
    elog(ERROR, "1: Error un-mmapping the file");
  }
  
  close(fd);
  
  // elog(NOTICE, "ReadGrid ------ FINISH\n");
 return blkno;
}

void ReadGrid2(Oid indexid )
{
  elog(NOTICE, "ReadGrid ------ start");
  
  //=================
  /* File Name */
  char * FullGridName = malloc(strlen(filepath)+sizeof(indexid) + 10 );
  sprintf(FullGridName , "%s%d.bin",filepath, indexid ) ; 

  //=================
  /* Open File */
  int fd;
  size_t filesize;
  unsigned char * fullMap;

  fd = open(FullGridName, O_RDONLY);
  if (fd == -1) {
    elog(ERROR, "1: Error opening file for reading");
  }

  elog(NOTICE, "ReadGrid ------ 1");

  //=================
  /* Map the file */
  filesize = sizeof(int) * MAX_GRID_ROWS * MAX_GRID_COLS + sizeof(double) * GRID_PARAMS ; 

  fullMap = (unsigned char *) mmap(0, filesize, PROT_READ, MAP_SHARED, fd, 0);
  if (fullMap == MAP_FAILED) {
    close(fd);
    elog(ERROR, "1: Error mmapping the file for grid params");
  }
  //=================
  /* Read Grid Params */
  int nrows =0 , ncols= 0;
  double minH , minW ;
  Point minPoint , maxPoint;
  
  int gridParamSize = sizeof(double) * GRID_PARAMS; 

  double *grid = (double *)fullMap;

  nrows = grid[0] ;//= r;
  ncols = grid[1] ;// = c;
  minW = grid[2]  ;//= minW;
  minH = grid[3]  ;//= minH;
  minPoint.x = grid[4];// = minP.x;
  minPoint.y = grid[5];// = minP.y;
  maxPoint.x = grid[6];// = maxP.x;
  maxPoint.y = grid[7];// = maxP.y;

 // if (munmap(grid, filesize) == -1) {
 //    elog(ERROR, "1: Error un-mmapping the file for grid params");
 //  }
  
  elog(NOTICE, "ReadGrid ------ 2 , Grid Params : %d\n%d\n%f\n%f\n%f\n%f\n%f\n%f",
                                           nrows, ncols, minW , minH , 
                                           minPoint.x, minPoint.y ,
                                           maxPoint.x , maxPoint.y);
  //=================
  /* realloc the map with the new sizes if necessaray */
  if(nrows > MAX_GRID_ROWS || ncols > MAX_GRID_COLS)
  {
    elog(NOTICE, "ReadGrid ----- un-mmapping the file for Remapping ");
    size_t old_size = filesize;
    filesize = sizeof(int) * nrows * ncols + sizeof(double) * GRID_PARAMS ; 
    
    if (munmap(fullMap, old_size) == -1) {
      elog(ERROR, "1: Error un-mmapping the file for Remapping ");
    }
    // fullMap = (unsigned char *)mremap (0, old_size, filesize);
    fullMap = (unsigned char *) mmap(0, filesize, PROT_READ, MAP_SHARED, fd, 0);
  }

  elog(NOTICE, "ReadGrid ------ 3 ");
  //=================
  /* Find the blkno */
  
  int *map;  /* mmapped array of int's */
  int blkno;
  size_t offset = sizeof(double) * GRID_PARAMS;
  
  // filesize = sizeof(int) * nrows * ncols + sizeof(double) * GRID_PARAMS; 
  elog(NOTICE, "ReadGrid ------ 4 ");
  map = (int *)(fullMap + offset);
  
    /* Read the file int-by-int from the mmap
     */
    printf("\n\nGridRead\n");
    int i ;
    int j ;
    for (i = 0; i <nrows; ++i) 
    {
     for (j = 0; j <ncols; ++j) 
     {
      printf( "%d ",map[i * (ncols) + j]);
     }
     printf( "\n\n");
    }
    
  if (munmap(fullMap, filesize) == -1) {
    elog(ERROR, "1: Error un-mmapping the file");
  }
  
  close(fd);
  
  elog(NOTICE, "ReadGrid ------ FINISH\n");
 // return blkno;
}

void writeToFile()
{
    int i;
    int fd;
    int result;
    int *map;  /* mmapped array of int's */

    /* Open a file for writing.
     *  - Creating the file if it doesn't exist.
     *  - Truncating it to 0 size if it already exists. (not really needed)
     *
     * Note: "O_WRONLY" mode is not sufficient when mmaping.
     */
    fd = open(FILEPATH, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (fd == -1) {
      elog(ERROR,"Error opening file for writing");
      // exit(EXIT_FAILURE);
    }

    /* Stretch the file size to the size of the (mmapped) array of ints
     */
    result = lseek(fd, FILESIZE-1, SEEK_SET);
    if (result == -1) {
      close(fd);
      elog(ERROR,"Error calling lseek() to 'stretch' the file");
      // exit(EXIT_FAILURE);
    }
    
    /* Something needs to be written at the end of the file to
     * have the file actually have the new size.
     * Just writing an empty string at the current file position will do.
     *
     * Note:
     *  - The current position in the file is at the end of the stretched 
     *    file due to the call to lseek().
     *  - An empty string is actually a single '\0' character, so a zero-byte
     *    will be written at the last byte of the file.
     */
    result = write(fd, "", 1);
    if (result != 1) {
      close(fd);
      elog(ERROR, "Error writing last byte of the file");
      // exit(EXIT_FAILURE);
    }

    /* Now the file is ready to be mmapped.
     */
    map = mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
    close(fd);
    elog(ERROR, "Error mmapping the file");
     // exit(EXIT_FAILURE);
    }
    
    /* Now write int's to the file as if it were memory (an array of ints).
     */
    for (i = 1; i <=NUMINTS; ++i) {
      map[i] = 2 * i; 
        }

    /* Don't forget to free the mmapped memory
     */
    if (munmap(map, FILESIZE) == -1) {
      elog(ERROR, "Error un-mmapping the file");
      /* Decide here whether to close(fd) and exit() or not. Depends... */
        }

    /* Un-mmaping doesn't close the file, so we still need to do that.
     */
    close(fd);
    // return 0;
}

void writeToFile2()
{

    
    int i, j;
    int fd;
    int result;
    // int *map[10];  /* mmapped array of int's */

    /* Open a file for writing.
     *  - Creating the file if it doesn't exist.
     *  - Truncating it to 0 size if it already exists. (not really needed)
     *
     * Note: "O_WRONLY" mode is not sufficient when mmaping.
     */
    fd = open(FILEPATH, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (fd == -1) {
      elog(ERROR,"Error opening file for writing");
      // exit(EXIT_FAILURE);
    }

    /* Stretch the file size to the size of the (mmapped) array of ints
     */
    result = lseek(fd, FILESIZE-1, SEEK_SET);
    if (result == -1) {
      close(fd);
      elog(ERROR,"Error calling lseek() to 'stretch' the file");
      // exit(EXIT_FAILURE);
    }
    
    /* Something needs to be written at the end of the file to
     * have the file actually have the new size.
     * Just writing an empty string at the current file position will do.
     *
     * Note:
     *  - The current position in the file is at the end of the stretched 
     *    file due to the call to lseek().
     *  - An empty string is actually a single '\0' character, so a zero-byte
     *    will be written at the last byte of the file.
     */
    result = write(fd, "", 1);
    if (result != 1) {
      close(fd);
      elog(ERROR, "Error writing last byte of the file");
      // exit(EXIT_FAILURE);
    }

    /* Now the file is ready to be mmapped.
     */
    int (*map)[10] = (int (*) [10])mmap(0, FILESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
    close(fd);
    elog(ERROR, "Error mmapping the file");
     // exit(EXIT_FAILURE);
    }
    
    /* Now write int's to the file as if it were memory (an array of ints).
     */
    for (i = 1; i <=NUMINTS; ++i) {
      for (j = 1; j <=NUMINTS; ++j) {
        map[i][j] = i * j; 
        }
      }

    /* Don't forget to free the mmapped memory
     */
    if (munmap(map, FILESIZE) == -1) {
      elog(ERROR, "Error un-mmapping the file");
      /* Decide here whether to close(fd) and exit() or not. Depends... */
        }

    /* Un-mmaping doesn't close the file, so we still need to do that.
     */
    close(fd);
    // return 0;
}

void ReadFromFile()
{
    int i;
    int fd;
    int *map;  /* mmapped array of int's */

    fd = open(FILEPATH, O_RDONLY);
    if (fd == -1) {
      elog(ERROR, "Error opening file for reading");
      // exit(EXIT_FAILURE);
    }

    map = mmap(0, FILESIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
      close(fd);
      elog(ERROR, "Error mmapping the file");
      // exit(EXIT_FAILURE);
    }
    
    /* Read the file int-by-int from the mmap
     */
    for (i = 1; i <=NUMINTS; ++i) {
      elog(NOTICE, "%d: %d\n", i, map[i]);
    }

    if (munmap(map, FILESIZE) == -1) {
      elog(ERROR, "Error un-mmapping the file");
    }
    close(fd);
    // return 0;
}
void ReadFromFile2()
{
    int i,j;
    int fd;
    // int *map[10];  /* mmapped array of int's */

    fd = open(FILEPATH, O_RDONLY);
    if (fd == -1) {
      elog(ERROR, "Error opening file for reading");
      // exit(EXIT_FAILURE);
    }

    int (* map) [10]=(int (*)[10]) mmap(0, FILESIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
      close(fd);
      elog(ERROR, "Error mmapping the file");
      // exit(EXIT_FAILURE);
    }
    
    /* Read the file int-by-int from the mmap
     */
    for (i = 1; i <=NUMINTS; ++i) {
      for (j = 1; j <=NUMINTS; ++j) {
      elog(NOTICE, "%d: %d", i, map[i][j]);
      }
    }

    if (munmap(map, FILESIZE) == -1) {
      elog(ERROR, "Error un-mmapping the file");
    }
    close(fd);
    // return 0;
}
//-------------------------------
//-------------------------------

int FindCost_catalogTbl(char * indexName, BlockNumber blkno, int k)
{
  char * relname = malloc(strlen(indexName) + 10 + sizeof(BlockNumber));
  sprintf(relname, "%s_CENTER_%d", indexName, blkno);

  /* Select * from tbl where K <= x; */
  char * SQL_SELECT = malloc(strlen(relname) + sizeof(k) + 50 );
  char Case = 'L'; // 'U' or 'L'. (upper part of the table, or lower)

  // if(k <= MAX_K/2)
  {
    // sprintf(SQL_SELECT , "SELECT * FROM %s WHERE K <= %d;", relname , k);
    // Case = 'U';
  }
  // else
  {
    sprintf(SQL_SELECT , "SELECT * FROM %s WHERE K >= %d;", relname , k);
    Case = 'L';
  }

  /* connect */
  if (SPI_connect() != SPI_OK_CONNECT)
    elog(ERROR, "SPI_connect failed");

  /* execute */
  int res = SPI_exec(SQL_SELECT  , 0);

  if(res < 0)
    elog(ERROR, "failed in Selection SQL Query = %s", SQL_SELECT);

  /* get the cost value */
  uint64    mySPI_processed = SPI_processed;
  int Cost = 1;

  if(SPI_tuptable != NULL )
  {
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    HeapTuple tuple;

    bool * isNull = palloc(sizeof(isNull));
    *(isNull) = false;
    int fnumer = 0;

    
    if(Case == 'U') // get the last element in the results
      fnumer = mySPI_processed-1 ;
  
     
    tuple = tuptable->vals[fnumer];
    
    // tuple->t_data;
    // (tuple)->t_data->t_hoff
    // tuple->t_infomask2;
    
    // (tupdesc)->attrs[(2)-1]
    // (tupdesc)->attrs[(2)-1]->attcacheoff;

    // elog(NOTICE, "tupleNoNulls%s" , HeapTupleNoNulls(tuple)? "true" : "false");
    // elog(NOTICE, "%d" , tuple->t_data );
    // elog(NOTICE, "valid tuple = %s" , (HeapTupleIsValid(tuple)? "true" : "false") );
    // elog(NOTICE, "%d" , tuple->t_data->t_hoff );
    // elog(NOTICE, "%d" , tuple->t_data->t_infomask2 );
    // elog(NOTICE, "%d" , (tupdesc)->attrs[(2)-1]);
    // elog(NOTICE, "%d" , (tupdesc)->attrs[(2)-1]->attcacheoff);


    Datum val = SPI_getbinval(tuple , tupdesc ,2 , isNull ); // Cost Value
    // Datum val = fastgetattr(tuple, 2, tupdesc, isNull);
    // heap_getsysattr
    Cost = DatumGetInt32(val);
  }


  /* close connection */
  if (SPI_finish() != SPI_OK_FINISH)
    elog(ERROR, "SPI_finish failed"); 

  return Cost;
}


// int FindCost_catalogTbl_Bin(char * indexName, BlockNumber blkno, int k)
int FindCost_catalogTbl_Bin(Oid indexoid, BlockNumber blkno, int k)
{
  // elog(NOTICE, "FindCost_catalogTbl_Bin ------ start");
  // elog(NOTICE, "Index Name = %d", indexoid);
  // char * path = malloc(sizeof(path) * 200);
  // path = "/Users/princess/Documents/Masters/code/kNNContribution/outputs/";

  char * relname = malloc(strlen(filepath) + sizeof(indexoid) + 10 + sizeof(BlockNumber));
  sprintf(relname, "%s%d%dC.bin", filepath,indexoid, blkno);
  // elog(NOTICE, "File Name = %s", relname);

  int i;
  int fd;
  int *map;  /* mmapped array of int's */
  int filesize = sizeof(int) * MAX_SIZE_KEY_CATALOG * 2+2; // ????

    fd = open(relname, O_RDONLY);
    if (fd == -1) {
      elog(ERROR, "1: Error opening file for reading");
    }
   
    //=================
    map = (int *) mmap(0, filesize, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
      close(fd);
      elog(ERROR, "1: Error mmapping the file");
    }
    
    //=================
    /* Read the file int-by-int from the mmap
     */
    int size = map[1];
  
    //===========================
    //===========================
    /* binary search*/
    int cost= 1;
    int StopLow = 2 , StopHigh = size*2+1;
    int val = 0;
    while(StopLow < StopHigh)
    {

      int StopMiddle = (StopLow + StopHigh) >>1;
      
      
      val = ((StopMiddle %2) == 0)? StopMiddle : StopMiddle+1; 
      
      int     middle = map[val];//DatumGetInt16(nodeLabels[StopMiddle]);

      if (k < middle)
        StopHigh = StopMiddle;
      else if (k > middle)
        StopLow = StopMiddle+1 ;
      else
      {
        
        val = ( (StopMiddle %2 )== 0) ? StopMiddle+1 : StopMiddle;
        cost = map[val];
        break;
      }
    }

    if(cost == 1)
    {
      val = ((StopHigh %2) == 0)? StopHigh+1 : StopHigh;
      cost = map[val];
    }


    //===========================
    //===========================
    

    if (munmap(map, filesize) == -1) {
      elog(ERROR, "1: Error un-mmapping the file");
    }
    
    close(fd);
    
    // elog(NOTICE, "FindCost_catalogTbl_Bin ------- FINISH\n");
 return cost;
}

//--------------------------------
//-------------------------------
void init_Block_arr()
{
  // elog(NOTICE, "init_Block_arr -------------- start");
  int blkno ;
  // int offset;
  for (blkno = 0; blkno < MAX_NO_LEAF_PAGE; blkno++)
    {
      Block_arr[blkno] = NULL;
    }
  minP.x = -1.0;
  minP.y = -1.0;
  maxP.x = 0.0;
  maxP.y = 0.0;
}


void ReadDataBlocks(Relation index , SpGistState *state)
{
  // elog(NOTICE, "ReadDataBlocks -------------- start");
  // printf("\n\nReadDataBlocks -------------- start\n");
  // create the first element
  stackItemData pageItem , *stackItem;
     
    // pgstat_count_index_scan(index);

    pageItem.blkno = SPGIST_ROOT_BLKNO;

    BlockIdSet(&(pageItem.ptr.ip_blkid), SPGIST_ROOT_BLKNO);
      
  pageItem.ptr.ip_posid = FirstOffsetNumber;
  pageItem.level = 0;
  pageItem.P_min.x = 0.0;
  pageItem.P_min.y = 0.0;

  // other initializations needed
  Buffer buffer = InvalidBuffer;
  
    List    *stack;
  // DataBlock_info *dataBlock_arr[MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
  // init_dataBlock_arr(dataBlock_arr);
  // dataBlock *Block_arr[MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
  init_Block_arr();  
  
  
  stack = NIL;
  
  // first element in the stack (root)
  stack = lcons(&pageItem, stack);  

  //DEBUG
  int pageCnt = 0;
  //DEBUG

  // start iterating over the index tree 
  while (true )
  {
    BlockNumber blkno;
    OffsetNumber offset;
    Page    page;
    // bool    isnull;
    
    /* Pull next to-do item from the list */
    if (stack == NIL)
      break;        /* there are no more pages to scan */

    CHECK_FOR_INTERRUPTS();

    stackItem = (stackItemData*)linitial(stack);
    stack = list_delete_first(stack);
  
  redirect:  
  
    /* Check for interrupts, just in case of infinite loop */
    CHECK_FOR_INTERRUPTS();

    blkno = ItemPointerGetBlockNumber(&stackItem->ptr);
    offset = ItemPointerGetOffsetNumber(&stackItem->ptr);

     //DEBUG
    // if(blkno == 133)
    // printf("read = %d , hit = %d\n",index->pgstat_info->t_counts.t_blocks_fetched ,  index->pgstat_info->t_counts.t_blocks_hit);
    //DEBUG

    if (buffer == InvalidBuffer)
    {
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }
    else if (blkno != BufferGetBlockNumber(buffer))
    {
      UnlockReleaseBuffer(buffer);
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }

    //DEBUG
    // if(blkno == 133)
    // printf("read = %d , hit = %d\n",index->pgstat_info->t_counts.t_blocks_fetched ,  index->pgstat_info->t_counts.t_blocks_hit);
    
    //DEBUG
    

    page = BufferGetPage(buffer);

    if (SpGistPageIsLeaf(page))
    {
      //DEBUG
      pageCnt++;

      //DEBUg

      //insert the item in the block array
      
        // Block_arr[blkno][offset] = malloc(sizeof( *Block_arr[blkno][offset]) ) ;
        
        // Block_arr[blkno][offset]->blkno = blkno;
        // Block_arr[blkno][offset]->offset = offset;
        // Block_arr[blkno][offset]->ptr = stackItem->ptr;
        // Block_arr[blkno][offset]->level = stackItem->level+1;
        // Block_arr[blkno][offset]->P_center = stackItem->P_center;
        // Block_arr[blkno][offset]->P_min.x = stackItem->P_min.x;
        // Block_arr[blkno][offset]->P_min.y = stackItem->P_min.y;
        // Block_arr[blkno][offset]->P_max.x = stackItem->P_max.x;
        // Block_arr[blkno][offset]->P_max.y = stackItem->P_max.y;

        // Block_arr[blkno][offset]->dist = 0.0;
        // Block_arr[blkno][offset]->dist_c1 = 0.0; 
        // Block_arr[blkno][offset]->dist_c2 = 0.0;
        // Block_arr[blkno][offset]->dist_c3 = 0.0;
        // Block_arr[blkno][offset]->dist_c4 = 0.0;
        if(blkno >= MAX_NO_LEAF_PAGE) 
          elog(ERROR, "blkno = %d exceeds the Block_arr size\n", blkno);
        
        if(Block_arr[blkno] == NULL)
        {
          Block_arr[blkno] = malloc(sizeof( *Block_arr[blkno]) ) ;
        
          Block_arr[blkno]->blkno = blkno;
          /* initialize offset array with zeros */
          int i;
          
          for(i =0 ; i< MAX_NO_LEAF_OFFSETS; i++)
            Block_arr[blkno]->offset[i] = 0;  

          if(offset >= MAX_NO_LEAF_OFFSETS) 
            elog(ERROR, "blkno = %d ,offset = %d exceeds the MAX_NO_LEAF_OFFSETS\n",blkno, offset);

          Block_arr[blkno]->offset[offset] = offset;
          Block_arr[blkno]->ptr[offset] = stackItem->ptr;
          Block_arr[blkno]->level = stackItem->level+1;
          

          Block_arr[blkno]->P_center = stackItem->P_center;
          Block_arr[blkno]->P_min.x = stackItem->P_min.x;
          Block_arr[blkno]->P_min.y = stackItem->P_min.y;
          Block_arr[blkno]->P_max.x = stackItem->P_max.x;
          Block_arr[blkno]->P_max.y = stackItem->P_max.y;

          Block_arr[blkno]->dist = 0.0;
          Block_arr[blkno]->dist_c1 = 0.0; 
          Block_arr[blkno]->dist_c2 = 0.0;
          Block_arr[blkno]->dist_c3 = 0.0;
          Block_arr[blkno]->dist_c4 = 0.0;
        }
        else
        {
          if(offset >= MAX_NO_LEAF_OFFSETS) 
            elog(ERROR, "blkno = %d ,offset = %d exceeds the MAX_NO_LEAF_OFFSETS\n",blkno ,offset);

          // Block_arr[blkno]->blkno = blkno;
          Block_arr[blkno]->offset[offset] = offset;
          Block_arr[blkno]->ptr[offset] = stackItem->ptr;
          // Block_arr[blkno]->level = stackItem->level+1;
          

          Block_arr[blkno]->P_center.x = (Block_arr[blkno]->P_center.x + stackItem->P_center.x)/2;
          Block_arr[blkno]->P_center.y = (Block_arr[blkno]->P_center.y + stackItem->P_center.y)/2;
          Block_arr[blkno]->P_min.x = min(Block_arr[blkno]->P_min.x , stackItem->P_min.x);
          Block_arr[blkno]->P_min.y = min(Block_arr[blkno]->P_min.y , stackItem->P_min.y);
          Block_arr[blkno]->P_max.x = max(Block_arr[blkno]->P_max.x , stackItem->P_max.x);
          Block_arr[blkno]->P_max.y = max(Block_arr[blkno]->P_max.y , stackItem->P_max.y);

        }
      
      //DEBUG

      //  if ( blkno == 289 || blkno == 288 || blkno == 113)
      // {
      //   printf("[%d,%d] (%f,%f)", blkno, offset ,Block_arr[blkno]->P_center.x , Block_arr[blkno]->P_center.y);
      //   printf("(%f,%f) , (%f,%f) \n" ,  Block_arr[blkno]->P_min.x , Block_arr[blkno]->P_min.y,
      //                                           Block_arr[blkno]->P_max.x , Block_arr[blkno]->P_max.y);
      // }
      // printf("Leaf: %d , %d\n",blkno, offset);
          // if (blkno == 133)
          //   printf("[%d,%d,%d] - (%f,%f),(%f,%f),(%f,%f)\n", 
          //          blkno ,offset, Block_arr[blkno]->level, Block_arr[blkno]->P_center.x , Block_arr[blkno]->P_center.y ,
          //                  Block_arr[blkno]->P_min.x , Block_arr[blkno]->P_min.y , 
          //                  Block_arr[blkno]->P_max.x , Block_arr[blkno]->P_max.y);

          // printf("\n Leaf page \n");
          // printf("%d , %d, %d -  %f , %f  \n",
          //   stackItem->level , blkno, offset, 
          //   stackItem->P_center.x , stackItem->P_center.y);
          //DEBUG
    }
    else /* Page is inner*/
    {

      SpGistInnerTuple innerTuple;
      // spgInnerConsistentIn in;
      // spgInnerConsistentOut out;
      // FmgrInfo   *procinfo;
      // SpGistNodeTuple *nodes;
      SpGistNodeTuple node;
      int     i;
      // *LeafReached = false;

      innerTuple = (SpGistInnerTuple) PageGetItem(page,
                    PageGetItemId(page, offset));

      if (innerTuple->tupstate != SPGIST_LIVE)
      {
        // DEBUG
        printf("\n\nLeaf tuple is NOT LIVE\n");
        //DEBUG
        if (innerTuple->tupstate == SPGIST_REDIRECT)
        {
          // DEBUG
          printf("Leaf tuple Redirect\n");
          //DEBUG

          /* transfer attention to redirect point */
          stackItem->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
          Assert(ItemPointerGetBlockNumber(&stackItem->ptr) != SPGIST_METAPAGE_BLKNO);
          goto redirect;
        }
        elog(ERROR, "unexpected SPGiST tuple state: %d",
           innerTuple->tupstate);
      }


      int which = 0; /* which quadrant this inner node lies in */

      Point* center = DatumGetPointP(SGITDATUM(innerTuple, state));
      Point min, max ; /* corner points of this inner node*/

      // elog(NOTICE, "spgistScanPage3 ----------- 6");

      if(stackItem->level == 0) // root Block
      {
        min.x = min.y = 0.0;
        max.x = ceil(2 * center->x);
        max.y = ceil(2 * center->y);
        // elog(NOTICE, "spgistScanPage3 ----------- 7");
      }
      else
      {
        /* get which quadrant this inner node lies in */
        which = getQuadrant( &stackItem->P_center , center);
        double diffx = fabs(center->x - stackItem->P_center.x);
        double diffy = fabs(center->y - stackItem->P_center.y);
        // elog(NOTICE, "spgistScanPage3 ----------- 8");
        switch(which)
        {
          case 1:
            min.x = min((stackItem->P_center.x) , center->x - diffx);
            min.y = min((stackItem->P_center.y) , center->y - diffy);
            max.x = max((stackItem->P_max.x) , center->x + diffx);
            max.y = max((stackItem->P_max.y) , center->y + diffy);
          break;
          case 2:
            min.x = min((stackItem->P_center.x) , center->x - diffx);
            min.y = min((stackItem->P_min.y), center->y - diffy);
            max.x = max((stackItem->P_max.x), center->x + diffx);
            max.y = max((stackItem->P_center.y), center->y + diffy);
          break;
          case 3:
            min.x = min((stackItem->P_min.x) , center->x - diffx);
            min.y = min((stackItem->P_min.y), center->y - diffy);
            max.x = max((stackItem->P_center.x), center->x + diffx);
            max.y = max((stackItem->P_center.y), center->y + diffy);
          break;
          case 4:
            min.x = min((stackItem->P_min.x) , center->x - diffx);
            min.y = min((stackItem->P_center.y), center->y - diffy);
            max.x = max((stackItem->P_center.x), center->x + diffx);
            max.y = max((stackItem->P_max.y), center->y + diffy);
          break;

          default:
          break;

        }

        //  switch(which)
        // {
        //   case 1:
        //     min.x = ( center->x - diffx);
        //     min.y = ( center->y - diffy);
        //     max.x = ( center->x + diffx);
        //     max.y = ( center->y + diffy);
        //   break;
        //   case 2:
        //     min.x = ( center->x - diffx);
        //     min.y = (center->y - diffy);
        //     max.x = ( center->x + diffx);
        //     max.y = ( center->y + diffy);
        //   break;
        //   case 3:
        //     min.x = ( center->x - diffx);
        //     min.y = ( center->y - diffy);
        //     max.x = ( center->x + diffx);
        //     max.y = ( center->y + diffy);
        //   break;
        //   case 4:
        //     min.x = ( center->x - diffx);
        //     min.y = ( center->y - diffy);
        //     max.x = ( center->x + diffx);
        //     max.y = ( center->y + diffy);
        //   break;

        //   default:
        //   break;

        // }

        //  switch(which)
        // {
        //   case 1:
        //     min.x = floor(stackItem->P_center.x);// , center->x - diffx);
        //     min.y = floor(stackItem->P_center.y);// , center->y - diffy);
        //     max.x = ceil(stackItem->P_max.x) ;//, center->x + diffx);
        //     max.y = ceil(stackItem->P_max.y) ;//, center->y + diffy);
        //   break;
        //   case 2:
        //     min.x = floor(stackItem->P_center.x);// , center->x - diffx);
        //     min.y = floor(stackItem->P_min.y);//, center->y - diffy);
        //     max.x = ceil(stackItem->P_max.x);//, center->x + diffx);
        //     max.y = ceil(stackItem->P_center.y);//, center->y + diffy);
        //   break;
        //   case 3:
        //     min.x = floor(stackItem->P_min.x);// , center->x - diffx);
        //     min.y = floor(stackItem->P_min.y);//, center->y - diffy);
        //     max.x = ceil(stackItem->P_center.x);//, center->x + diffx);
        //     max.y = ceil(stackItem->P_center.y);//, center->y + diffy);
        //   break;
        //   case 4:
        //     min.x = floor(stackItem->P_min.x);// , center->x - diffx);
        //     min.y = floor(stackItem->P_center.y);//, center->y - diffy);
        //     max.x = ceil(stackItem->P_center.x);//, center->x + diffx);
        //     max.y = ceil(stackItem->P_max.y);//, center->y + diffy);
        //   break;

        //   default:
        //   break;

        // }
      }

      // printf("Inner:\n");
      // printf("Inner: %d , %d\n",blkno, offset);
      // loop over the 4 quadrants (children) of the current inner node
        SGITITERATE(innerTuple, i, node)
      {
        
        if (ItemPointerIsValid(&node->t_tid))
        {

          /* create the spGistSearchItem to be inserted in the queue*/
          stackItemData *item;
          item = palloc(sizeof(stackItemData));

           /* Creating heap-tuple GISTSearchItem */
          item->blkno = ItemPointerGetBlockNumber(&node->t_tid);
          // int d = ItemPointerGetOffsetNumber(&node->t_tid);
          // printf("child[%d]: %d , %d\n" ,i , item->blkno , d);
          // item->data.heap.heapPtr = node->t_tid;
          item->ptr = node->t_tid;
          
          item->level = stackItem->level+1;
          item->P_center = *center;
          item->P_min.x = min.x;
          item->P_min.y = min.y;
          item->P_max.x = max.x;
          item->P_max.y = max.y;

          // insert it in the stack
          stack = lcons(item, stack);
          
        }
      }

      
      // printf("%d , %d , %d,  %f , %f  ,  %f , %f  ,  %f , %f \n",
      //       stackItem->level , blkno, offset,
      //              center->x , center->y,
      //              min.x , min.y ,
      //              max.x , max.y);
    }
  }
  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);
  // pfree(stack);
  //DEBUG
  // printf("\n\nReadDataBlock: pageCnt = %d\n\n" , pageCnt);
  // printf("\n\nReadDataBlocks -------------- Finish\n");
  //DEBUg
} 

void BuildCatalogLogic(SpGistState *state , Relation index)
{
  // elog(NOTICE , "BuildCatalogLogic -------- start:\n");
  pairingheap* blockQ; // create the prioirity queue for blocks
  // Point * center;
  // dataBlock *b ;
  int blkno = 0;
  // int offset;

  //debug
  int cnt = 0;
  //debug
  for(; blkno < MAX_NO_LEAF_PAGE; blkno++)
  { 
    // for(offset = 0; offset < MAX_NO_LEAF_PAGE; offset++)
    {
      if(Block_arr[blkno] == NULL)
        continue;

      cnt ++;
      blockQ=NULL;
      blockQ = pairingheap_allocate(pairingheap_dataBlockCenter_cmp, NULL);

      //DEBUG
       // printf("[%d,%d] (%f,%f) <-> " , blkno , offset , Block_arr[blkno][offset]->P_center.x , Block_arr[blkno][offset]->P_center.y);
      //DEBUG

      //STEP1: fill the BlockQ with MinDist according to the center of this block
      fill_blockQ(blockQ , Block_arr[blkno]->P_center ); 
      // fill_blockQ(blockQ , Block_arr[blkno]->P_min ); 

      //DEBUG
      // print_block_arr( index ,  state);
      // print_pairingheap(blockQ);
      //DEBUG
      
      //STEP2: Fill the catalog-center for this data block
      Fill_catalog_center(blockQ ,blkno ,0 , Block_arr[blkno]->P_center ,index,  state);
      // Fill_catalog_center(blockQ ,blkno ,0 , Block_arr[blkno]->P_min ,index,  state);

      //DEBUG
      // elog(NOTICE , "Block = %d", blkno);
      // print_catalog(&Block_arr[blkno]->catalog_center);
      //DEBUG

      // ======================== freeing memory
      pairingheap_reset(blockQ);
      // TODO : fill the BlockQ with MinDist according to the Corner UL point
      // TODO : fill the BlockQ with MinDist according to the Corner UR point
      // TODO : fill the BlockQ with MinDist according to the Corner LL point
      // TODO : fill the BlockQ with MinDist according to the Corner LR point
    }
  }
}

// for comparing datablocks w.r.t each other (distance between center of datablock and the min distance in such block)
static int
pairingheap_dataBlockCenter_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
  const DataBlockHeap_info *sa = (const DataBlockHeap_info *) a;
  const DataBlockHeap_info *sb = (const DataBlockHeap_info *) b;
  
  /* Order according to distance comparison */
  if (sa->dist != sb->dist)
      return (sa->dist < sb->dist) ? 1 : -1;


  return 0;
}

/*
 * which = 0 -> center 
 * which = 1 -> UL  Upper Left
 * which = 2 -> UR. Upper Right
 * which = 3 -> LL  Lower Left
 * which = 4 -> LR. Lower Right
*/
void fill_blockQ(pairingheap * blockQ , Point q ) 
{
  // elog(NOTICE, "Fill BlockQ function is called");
  // printf("\n------------------------------------fill_blockQ:\n");
  int blkno = 0;
  // int offset=0;
  for(; blkno < MAX_NO_LEAF_PAGE ; blkno++)
    {
      // for(offset=0; offset < MAX_NO_LEAF_PAGE ; offset++)
      {
        if(Block_arr[blkno] == NULL)
          continue;
        
          BOX box;
          box.low = Block_arr[blkno]->P_min;
          box.high = Block_arr[blkno]->P_max;

          //DEBUG
          
          // printf("[%d] , (%f,%f) , (%f,%f) \n" , blkno , box.low.x , box.low.y , box.high.x , box.high.y);
          //DEBUG
          
          Block_arr[blkno]->dist = computeDistance( false,  &box, &q); // distqnce between a block and a point
          
          // DEBUG
          // if(blkno == 289 || blkno == 288)
          //   printf("Fill_BlockQ : %f ,[%d,%d] , (%f,%f) , (%f ,%f)\n" , Block_arr[blkno]->dist, blkno , offset , box.low.x , box.low.y, box.high.x , box.high.y );
          // printf("blkno = %d , dist = %f \n" , blkno , Block_arr[blkno]->dist);
          // printf("box ( %f , %f ) , ( %f , %f )  -  q ( %f , %f ) \n", 
          //        box.low.x , box.low.y, box.high.x , box.high.y ,q.x , q.y);
          // if(blkno == 133)
          //   elog(NOTICE , "blkno = 132");
          // DEBUG

          
          //========== fill the priority queue 
          DataBlockHeap_info * block;
          block = palloc(sizeof(*block));
          block->dist = Block_arr[blkno]->dist;
          block->blkno = blkno;
          // block->offset = offset;
          //elog(NOTICE, "add pairingheap node");
          //pairingheap_add(blockQ, &(dataBlock_arr[blkno]->phNode));
          pairingheap_add(blockQ, &(block->phNode));
          //elog(NOTICE, "add pairingheap node finish");
        
          //elog(NOTICE, " <-> %d = %f" ,  blkno, dist);
      }  
      
    }
}


void Fill_catalog_center(pairingheap* blockQ,  int blkno , int offset, Point q ,Relation index,  SpGistState *state)
{
  //DEBUG
  // elog(NOTICE, "Fill_catalog_center --------- start, blkno = %d\n", blkno);
  // printf("\n\n------------------------------------fill_catalog_center:\n\n");
  // printf("[%d,%d]  (%f,%f) <->  " , blkno , offset, q.x , q.y);
  //DEBUG

  pairingheap* tupleQ; // create the prioirity queue for tuples
  tupleQ = pairingheap_allocate(pairingheap_TuplePointInfo_cmp, NULL);
  
  // elog(NOTICE, "Fill_catalog_center --------- 1\n");

  //insanity check 
  if(Block_arr[blkno] == NULL)
    elog(ERROR, "Fill_catalog_center: Block_arr[%d] is NULL\n", blkno);

  Block_arr[blkno]->catalog_center.size = 0;
  int cost = 0, currentK=1, startK = 0 , prevK = 1;;
  
  DataBlockHeap_info * dataBlockHeap = NULL , *blockQ_top = NULL;
  TuplePoint_info * tupleQ_top = NULL;
  int  catalog_iter=1;
  
  while(currentK <= MAX_K)
  {
    // elog(NOTICE, "Fill_catalog_center --------- 2\n");
    // retrieve the top of the queue
    dataBlockHeap = NULL;
    dataBlockHeap = getNextDataBlock(blockQ);
    
    // elog(NOTICE, "Fill_catalog_center --------- 2\n");

    if(dataBlockHeap) // NULL means no more datablocks avaialble
    {
      int blkno_cur = dataBlockHeap->blkno;
      int offset_cur = 0;//dataBlockHeap->offset;
      
      //DEBUG
      // if(blkno == 288 || blkno == 289)
      // printf("[%d,%d] dist= %f\n", blkno_cur , offset_cur , dataBlockHeap->dist);
      //DEBUG
      cost++;
      
      // elog(NOTICE, "Fill_catalog_center --------- 3\n");
      FillTupleQ(tupleQ , blkno_cur, offset_cur , q, index, state);
      // elog(NOTICE, "Fill_catalog_center --------- 4\n");

      startK = currentK;

      blockQ_top  = NULL;
      blockQ_top  = DataBlock_top(blockQ);
      // elog(NOTICE, "Fill_catalog_center --------- 5\n");
  
      //DEBUG
        // if(tupleQ_top && blockQ_top)
        //   printf("tuple: [%f] ( %f, %f )" , tupleQ_top->dist , tupleQ_top->p.x , tupleQ_top->p.y);
        //   printf("block: [%f] [%d,%d]\n" , blockQ_top->dist , blockQ_top->blkno , blockQ_top->offset );
      //DEBUG
      while(blockQ_top!= NULL && blockQ_top->dist == 0)
      {
        // elog(NOTICE, "Fill_catalog_center --------- 6\n");
        dataBlockHeap = NULL;
        dataBlockHeap = getNextDataBlock(blockQ);
        // elog(NOTICE, "Fill_catalog_center --------- 7\n");
        
        if(dataBlockHeap) // NULL means no more datablocks avaialble
        {
          // elog(NOTICE, "Fill_catalog_center --------- 8\n");
          int blkno_cur2 = dataBlockHeap->blkno;
    
          FillTupleQ(tupleQ , blkno_cur2, offset_cur , q, index, state);

          // elog(NOTICE, "Fill_catalog_center --------- 9\n");
          blockQ_top  = NULL;
          blockQ_top  = DataBlock_top(blockQ);
          // elog(NOTICE, "Fill_catalog_center --------- 10\n");
          //DEBUG
          // if(blkno == 288 || blkno == 289)
          // printf("[%d,%d] dist= %f\n", blkno_cur2 , offset_cur , dataBlockHeap->dist);
          //DEBUG
        }
      }
      // elog(NOTICE, "Fill_catalog_center --------- 11\n");
      tupleQ_top  = NULL;
      tupleQ_top  = Tuple_top(tupleQ);
      // elog(NOTICE, "Fill_catalog_center --------- 12\n");
      //DEBUG
        // if(tupleQ_top && blockQ_top && (blkno == 288 || blkno == 289))
        // {
        //   printf("tuple: [%f] ( %f, %f )" , tupleQ_top->dist , tupleQ_top->p.x , tupleQ_top->p.y);
        //   printf("block: [%f] [%d,%d]\n" , blockQ_top->dist , blockQ_top->blkno , blockQ_top->offset );
        // }
      //DEBUG
      while( blockQ_top != NULL && tupleQ_top != NULL && tupleQ_top->dist <= blockQ_top->dist )
      {
        /* remove the top tuple in tupleQ */
        tupleQ_top = getNextTuple(tupleQ);
        // elog(NOTICE, "Fill_catalog_center --------- 13\n");
        currentK++;

        // blockQ_top  = DataBlock_top(blockQ);
        tupleQ_top = NULL;
        tupleQ_top  = Tuple_top(tupleQ);
        // elog(NOTICE, "Fill_catalog_center --------- 14\n");
      }

      /* add this row in the catalog */
      if(currentK == prevK) // no tuples incremented 
      {
        add_newItem_Catalog(&Block_arr[blkno]->catalog_center, cost , currentK , catalog_iter);  
        // elog(NOTICE, "Fill_catalog_center --------- 15\n");
      }
      else
      {
        add_newItem_Catalog(&Block_arr[blkno]->catalog_center, cost , currentK , catalog_iter);
        // elog(NOTICE, "Fill_catalog_center --------- 16\n");
        catalog_iter++;
        prevK = currentK;
      }
    }
    else if (Tuple_top(tupleQ) != NULL ) /* no more datablocks in the blockQ but there is tuples in the TupleQ */
    {
      // elog(NOTICE, "Fill_catalog_center --------- 17\n");
      /* remove the top tuple in tupleQ */
      tupleQ_top = getNextTuple(tupleQ);
      // elog(NOTICE, "Fill_catalog_center --------- 18\n");
      currentK++;
    }
    else
    {
      // elog(NOTICE, "Fill_catalog_center --------- 19\n");
      /* add this row in the catalog */
      cost++;
      if(currentK == prevK)
      {
        add_newItem_Catalog(&Block_arr[blkno]->catalog_center, cost , currentK , catalog_iter);
        // elog(NOTICE, "Fill_catalog_center --------- 20\n");
      }
      else
      {  
        add_newItem_Catalog(&Block_arr[blkno]->catalog_center, cost , currentK , catalog_iter);
        // elog(NOTICE, "Fill_catalog_center --------- 21\n");
        catalog_iter++;
        prevK = currentK;
      }
      break;
    }
  }
  // elog(NOTICE, "Fill_catalog_center --------- 22\n");
  pairingheap_reset(tupleQ);
  // elog(NOTICE, "Fill_catalog_center --------- END\n\n");
}

static DataBlockHeap_info * DataBlock_top(pairingheap* blockQ)
{
  DataBlockHeap_info * item;
  if (blockQ != NULL && !pairingheap_is_empty(blockQ))
  {
    item = (DataBlockHeap_info *) pairingheap_first(blockQ);
  }
  else
  {
    /* Done when both heaps are empty */
    item = NULL;
  }
  return item;
}


static DataBlockHeap_info * getNextDataBlock(pairingheap* blockQ)
{
  DataBlockHeap_info * item;
  if (blockQ != NULL && !pairingheap_is_empty(blockQ))
  {
    item = (DataBlockHeap_info *) pairingheap_remove_first(blockQ);
  }
  else
  {
    /* Done when both heaps are empty */
    item = NULL;
  }
  return item;
}

static TuplePoint_info * getNextTuple(pairingheap* tupleQ)
{
  TuplePoint_info * item;
  if (tupleQ != NULL && !pairingheap_is_empty(tupleQ))
  {
    item = (TuplePoint_info *) pairingheap_remove_first(tupleQ);
  }
  else
  {
    /* Done when both heaps are empty */
    item = NULL;
  }
  return item;
}

static TuplePoint_info * Tuple_top(pairingheap* tupleQ)
{
  TuplePoint_info * item;
  if (tupleQ != NULL && !pairingheap_is_empty(tupleQ))
  {
    item = (TuplePoint_info *) pairingheap_first(tupleQ);
  }
  else
  {
    /* Done when both heaps are empty */
    item = NULL;
  }
  return item;
}

static int
pairingheap_TuplePointInfo_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
  const TuplePoint_info *sa = (const TuplePoint_info *) a;
  const TuplePoint_info *sb = (const TuplePoint_info *) b;
  
  /* Order according to distance comparison */
  if (sa->dist != sb->dist)
      return (sa->dist < sb->dist) ? 1 : -1;


  return 0;
}

void FillTupleQ(pairingheap* tupleQ , int p_blkno, int p_offset , Point q, Relation index, SpGistState *state)
{
  // printf("\n------------------------------------Fill TupleQ:\n\n");
  // elog(NOTICE, "FillTupleQ ----------------- start\n");
  // get the pointer of this datablock and retrive all its points 
  Buffer buffer = InvalidBuffer;
  BlockNumber blkno;
  OffsetNumber offset;
  Page    page;

  //DEBUG
  int  PointCnt = 0;
  //DEBUG

  for(p_offset = 0 ; p_offset < MAX_NO_LEAF_OFFSETS; p_offset++)
  {
    if(Block_arr[p_blkno] == NULL) 
      elog(ERROR, "FillTupleQ: Block_arr[%d] is NULL\n", p_blkno);
    
    if(Block_arr[p_blkno]->offset[p_offset] == 0)
      continue;
    
    // elog(NOTICE, "FillTupleQ ----------------- 1");
    blkno = ItemPointerGetBlockNumber(&Block_arr[p_blkno]->ptr[p_offset]);
    offset = ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]);

    // elog(NOTICE, "FillTupleQ ----------------- 2");
    // insanity check 
    Assert((blkno == p_blkno));

    // elog(NOTICE, "FillTupleQ ----------------- 3");
    if (buffer == InvalidBuffer)
    {
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }
    else if (blkno != BufferGetBlockNumber(buffer))
    {
      UnlockReleaseBuffer(buffer);
      buffer = ReadBuffer(index, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }

    // elog(NOTICE, "FillTupleQ ----------------- 4");
    page = BufferGetPage(buffer);
    
    // elog(NOTICE, "FillTupleQ ----------------- 5");

    if (SpGistPageIsLeaf(page)) 
    {
      SpGistLeafTuple leafTuple;
      OffsetNumber max = PageGetMaxOffsetNumber(page);
      
      //DEBUG
      // elog(NOTICE, "FillTupleQ ----------------- 6");
      // POLYGON *dataBlock_tmp;
      // char * result;
      // result = palloc(sizeof(result) *50 * max*2);
      // strcpy(result,"(");
      // bool cont = false;
      // int count_cont = 0;
      // printf("Blkno = %d - offset = %d\n", blkno, offset );
      // printf("%f,%f,%f,%f\n", Block_arr[p_blkno]->P_min.x, Block_arr[p_blkno]->P_min.y,
                                   // Block_arr[p_blkno]->P_max.x, Block_arr[p_blkno]->P_max.y);
      // printf("\n------------------------------------\n");
      //DEBUG

      if (SpGistBlockIsRoot(blkno))
      {
        // elog(NOTICE, "FillTupleQ ----------------- 7");
        /* When root is a leaf, examine all its tuples */
          for (offset = FirstOffsetNumber; offset <= max; offset++)
          {
            leafTuple = (SpGistLeafTuple)
              PageGetItem(page, PageGetItemId(page, offset));
            if (leafTuple->tupstate != SPGIST_LIVE)
            {
              /* all tuples on root should be live */
              elog(ERROR, "unexpected SPGiST tuple state: %d",
                 leafTuple->tupstate);
            }

            Assert(ItemPointerIsValid(&leafTuple->heapPtr));

            /* retrieve the point */
            Point *p = DatumGetPointP(SGLTDATUM(leafTuple, state));
            
            /* compute the distance */
            BOX box;
            box.low = box.high  = *p;

            double dist = computeDistance( true,  &box, &q); // distqnce between a block and a point
            
            /* insert in the tupleQ */
            TuplePoint_info * tuple;
            tuple = malloc (sizeof(TuplePoint_info));
            tuple->dist= dist;
            tuple->p.x = p->x;
            tuple->p.y = p->y;

            // insert into the queue
            pairingheap_add(tupleQ, &(tuple->phNode));

            // For Grid Implementation 
            if(minP.x == -1) // first time
              minP = *p;
            else 
            {
              if(minP.x > p->x)
                minP.x = p->x;
              if(minP.y > p->y)
                minP.y = p->y;
            }

            if(maxP.x == -1)
              maxP = *p;
            else
            {
              if(maxP.x < p->x)
                maxP.x = p->x;
              if(maxP.y < p->y)
                maxP.y = p->y;
            }

            //DEBUG
            // printf("%f,%f\n", p->x, p->y);
            //DEBUG
          }
      }
      else
      {
        // elog(NOTICE, "FillTupleQ ----------------- 8");
        /* Normal case: just examine the chain we arrived at */
        while (offset != InvalidOffsetNumber)
        {
          // elog(NOTICE, "FillTupleQ ----------------- 9");
          
          Assert(offset >= FirstOffsetNumber && offset <= max);
          leafTuple = (SpGistLeafTuple)
            PageGetItem(page, PageGetItemId(page, offset));
          
          // elog(NOTICE, "FillTupleQ ----------------- 10");
          if (leafTuple->tupstate != SPGIST_LIVE)
          {
            // DEBUG
            printf("\n\nLeaf tuple is NOT LIVE\n");
            // count_cont++;//for debug
            // cont=true;
            //DEBUG
            if (leafTuple->tupstate == SPGIST_REDIRECT)
            {
              // DEBUG
              printf("Leaf tuple Redirect\n");
              //DEBUG

              /* redirection tuple should be first in chain */
              Assert(offset == ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]));
              /* transfer attention to redirect point */
              Block_arr[p_blkno]->ptr[p_offset] = ((SpGistDeadTuple) leafTuple)->pointer;
              Assert(ItemPointerGetBlockNumber(&Block_arr[p_blkno]->ptr[p_offset]) != SPGIST_METAPAGE_BLKNO);
              // goto redirect;
            }
            if (leafTuple->tupstate == SPGIST_DEAD)
            {
              // DEBUG
              printf("Leaf tuple is DEAD\n");
              //DEBUG

              /* dead tuple should be first in chain */
              Assert(offset == ItemPointerGetOffsetNumber(&Block_arr[p_blkno]->ptr[p_offset]));
              /* No live entries on this page */
              Assert(leafTuple->nextOffset == InvalidOffsetNumber);
              break;
            }
            /* We should not arrive at a placeholder */
            elog(ERROR, "unexpected SPGiST tuple state: %d",
               leafTuple->tupstate);
          }

          // elog(NOTICE, "FillTupleQ ----------------- 11");
          Assert(ItemPointerIsValid(&leafTuple->heapPtr));

          // elog(NOTICE, "FillTupleQ ----------------- 12");
          
          /* retrieve the point */
          Point *p = DatumGetPointP(SGLTDATUM(leafTuple, state));
          
          // elog(NOTICE, "FillTupleQ ----------------- 13");
          /* compute the distance */
          BOX box;
          box.low = box.high  = *p;

          double dist = computeDistance( true,  &box, &q); // distqnce between a block and a point
          
          // elog(NOTICE, "FillTupleQ ----------------- 14");
          /* insert in the tupleQ */
          TuplePoint_info * tuple;
          tuple = malloc (sizeof(TuplePoint_info));
          tuple->dist= dist;
          tuple->p.x = p->x;
          tuple->p.y = p->y;

          // insert into the queue
          pairingheap_add(tupleQ, &(tuple->phNode));
          // elog(NOTICE, "FillTupleQ ----------------- 15");
          offset = leafTuple->nextOffset;

          // For Grid Implementation 
            if(minP.x == -1) // first time
              minP = *p;
            else 
            {
              if(minP.x > p->x)
                minP.x = p->x;
              if(minP.y > p->y)
                minP.y = p->y;
            }

            if(maxP.x == -1)
              maxP = *p;
            else
            {
              if(maxP.x < p->x)
                maxP.x = p->x;
              if(maxP.y < p->y)
                maxP.y = p->y;
            }

          //DEBUG
          PointCnt ++;
          //DEBUG
            // printf("%f,%f\n", p->x, p->y);
            //DEBUG
          // Fill the array of points for Bounding box
          // char str1[30];
          // snprintf(str1, sizeof(str1),"%f", p->x);
          // strcat(result, str1);
          // strcat(result,",");

          // snprintf(str1, sizeof(str1),"%f", p->y);
          // strcat(result, str1);
          
          // if(offset != InvalidOffsetNumber)
          //   strcat(result, "),(");

          // if(blkno == 104 || blkno == 113 || blkno == 123)
          //   printf("[%d,%d] Points: (%f,%f)  %f\n", blkno , offset , p->x , p->y , dist);
          //DEBUG
        }

        // elog(NOTICE, "FillTupleQ ----------------- 16");
        //DEBUG
        // printf("[%d,%d] max = %d , pointCnt = %d \n", blkno ,offset, max , PointCnt);
        PointCnt = 0;
        // if(cont)
        // {
        //   // remove the last comma
        //   int size = strlen(result);
        //   result[size-2] = '\0';
        // }
        // else
        // strcat(result, ")");
        // // build a Polygon for the points in a data block (Bounding box)
        // dataBlock_tmp = DatumGetPolygonP(DirectFunctionCall1Coll(poly_in,InvalidOid,result));
        // Point * center2 = DatumGetPointP(DirectFunctionCall1Coll(poly_center,InvalidOid,PolygonPGetDatum(dataBlock_tmp)));
        // printf("[%d] #%d (%f,%f) (%f,%f) (%f,%f)\n" , blkno , dataBlock_tmp->npts , 
        //                                    center2->x , center2->y ,
        //                                    dataBlock_tmp->boundbox.low.x, dataBlock_tmp->boundbox.low.y,
        //                                    dataBlock_tmp->boundbox.high.x, dataBlock_tmp->boundbox.high.y);
        //DEBUG

      }
      //DEBUG
       // printf("\n------------------------------------\n");
      //DEBUG      
    }
    else
    {
      elog(ERROR , "This page should be leaf not Inner ... there is a Problem\n");
    }
  }
  // elog(NOTICE, "FillTupleQ ----------------- 17");
  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);


  // elog(NOTICE, "FillTupleQ ----------------- END");
}





void print_catalog(CATALOG* _catalog)
{
  int i=0;
  elog(NOTICE, "Kend\t|\tcost ");
  for (; i< _catalog->size; i++)
    elog(NOTICE, ",%d\t,\t%d " , _catalog->key[i] , _catalog->cost[i]);
  // print the last two catalogs
  
}


void add_newItem_Catalog(CATALOG* _catalog, int cost , int key , int size)
{
  if(size-1 < 0 || size > MAX_SIZE_KEY_CATALOG )
    elog(ERROR, "catalog size %d is excedded" , size);
  _catalog->key[size-1] = key;
  _catalog->cost[size-1] = cost;
  _catalog->size = size;
}

/* ==========================================================
 *     Find the number pages required to be scanned 
 *     correspond to specific blkno and K 
 *    This function should scan the block_arr in a binary search 
 * ========================================================== */
int FindCost_catalog(int blkno , int k)
{
  // bool found = false;
  int foundIndex = 0;
  if(searchCatalog(&Block_arr[blkno]->catalog_center, k, &foundIndex))
    return Block_arr[blkno]->catalog_center.cost[foundIndex];
  else
    return 0;

}

static bool
searchCatalog(CATALOG* _catalog, int k, int *i)
{
  int     StopLow = 0,
        StopHigh = _catalog->size;

  while (StopLow < StopHigh)
  {
    int     StopMiddle = (StopLow + StopHigh) >> 1;
    int     middle = _catalog->key[StopMiddle];//DatumGetInt16(nodeLabels[StopMiddle]);

    if (k < middle)
      StopHigh = StopMiddle;
    else if (k > middle)
      StopLow = StopMiddle + 1;
    else
    {
      *i = StopMiddle;
      return true;
    }
  }

  // *i = StopMiddle;
  *i = StopHigh;
  return true;
}
//----------------------------

/* ==========================================================
 *    My Quadtree Implementation for auxiliary index
 * ========================================================== */

int
myspgWalk(Oid oid , Point * queryPoint, char * indexName)
{
  // elog(NOTICE, "myspgWalk index for this index Relation\n");
  #if PG_VERSION_NUM < 90200
  elog(NOTICE, "Function is not working under PgSQL < 9.2");

  return 0;
  // PG_RETURN_TEXT_P(CStringGetTextDatum("???"));
  #else
  // #define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX) // rd_rel relation typle from pg_class
  // #define IS_SPGIST(r) ((r)->rd_rel->relam == SPGIST_AM_OID)
  // #define SPGIST2_AM_OID 65731
  // #define IS_SPGIST2(r) ((r)->rd_rel->relam == SPGIST2_AM_OID)
  //==============================  Open Index table  ======================================================

  //text      *name=PG_GETARG_TEXT_P(0);
  // Oid oid = PG_GETARG_UINT32(0);

  // RangeVar    *relvar;
  Relation    index_rel;
  // Point * queryPoint = PG_GETARG_POINT_P(1);
  // ItemPointerData ipd;
  
  // relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
  // index = relation_openrv(relvar, AccessExclusiveLock);
  // index_rel = try_relation_open(oid, AccessExclusiveLock);
  index_rel = relation_open(oid, AccessExclusiveLock);

  if (!IS_INDEX(index_rel) || (!IS_SPGIST(index_rel) && !IS_SPGIST2(index_rel)))
    elog(ERROR, "relation \"%s\" is not an SPGiST index",
       RelationGetRelationName(index_rel));
  //elog(NOTICE, "free (name)");
  // pfree(name);
  
  indexName = strcpy( indexName ,RelationGetRelationName(index_rel));
  /* -------------------------------------  
   * Find the blkno of the query point  
   * ------------------------------------- */
   SpGistState state;
   initSpGistState(&state, index_rel);
  

   int blkno = FindPoint( index_rel , &state , queryPoint);
  
  

  index_close(index_rel, AccessExclusiveLock);
  index_rel = NULL;
  
  return blkno;
  #endif
}

// void my_spg_quad_inner_consistent(spgInnerConsistentIn *in, spgInnerConsistentOut *out)
// {
//   // spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
//   // spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
//   Point    *centroid;
//   int     which;
//   int     i;

//   Assert(in->hasPrefix);
//   centroid = DatumGetPointP(in->prefixDatum);

//   if (in->allTheSame)
//   {
//     /* Report that all nodes should be visited */
//     out->nNodes = in->nNodes;
//     out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
//     for (i = 0; i < in->nNodes; i++)
//       out->nodeNumbers[i] = i;
//     // PG_RETURN_VOID();
//     return;
//   }

//   Assert(in->nNodes == 4);

//   /* "which" is a bitmask of quadrants that satisfy all constraints */
//   which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);

//   for (i = 0; i < in->nkeys; i++)
//   {
//     Point    *query = DatumGetPointP(in->scankeys[i].sk_argument);
//     BOX      *boxQuery;

//     switch (in->scankeys[i].sk_strategy)
//     {
//       case RTLeftStrategyNumber:
//         if (SPTEST(point_right, centroid, query))
//           which &= (1 << 3) | (1 << 4);
//         break;
//       case RTRightStrategyNumber:
//         if (SPTEST(point_left, centroid, query))
//           which &= (1 << 1) | (1 << 2);
//         break;
//       case RTSameStrategyNumber:
//         which &= (1 << getQuadrant(centroid, query));
//         break;
//       case RTBelowStrategyNumber:
//         if (SPTEST(point_above, centroid, query))
//           which &= (1 << 2) | (1 << 3);
//         break;
//       case RTAboveStrategyNumber:
//         if (SPTEST(point_below, centroid, query))
//           which &= (1 << 1) | (1 << 4);
//         break;
//       case RTContainedByStrategyNumber:

//         /*
//          * For this operator, the query is a box not a point.  We
//          * cheat to the extent of assuming that DatumGetPointP won't
//          * do anything that would be bad for a pointer-to-box.
//          */
//         boxQuery = DatumGetBoxP(in->scankeys[i].sk_argument);

//         if (DatumGetBool(DirectFunctionCall2(box_contain_pt,
//                            PointerGetDatum(boxQuery),
//                          PointerGetDatum(centroid))))
//         {
//           /* centroid is in box, so all quadrants are OK */
//         }
//         else
//         {
//           /* identify quadrant(s) containing all corners of box */
//           Point   p;
//           int     r = 0;

//           p = boxQuery->low;
//           r |= 1 << getQuadrant(centroid, &p);
//           p.y = boxQuery->high.y;
//           r |= 1 << getQuadrant(centroid, &p);
//           p = boxQuery->high;
//           r |= 1 << getQuadrant(centroid, &p);
//           p.x = boxQuery->low.x;
//           r |= 1 << getQuadrant(centroid, &p);

//           which &= r;
//         }
//         break;
//       default:
//         elog(ERROR, "unrecognized strategy number: %d",
//            in->scankeys[i].sk_strategy);
//         break;
//     }

//     if (which == 0)
//       break;        /* no need to consider remaining conditions */
//   }

//   /* We must descend into the quadrant(s) identified by which */
//   out->nodeNumbers = (int *) palloc(sizeof(int) * 4);
//   out->nNodes = 0;
//   for (i = 1; i <= 4; i++)
//   {
//     if (which & (1 << i))
//       out->nodeNumbers[out->nNodes++] = i - 1;
//   }

//   // PG_RETURN_VOID();
//   return;
// }

int FindPoint(Relation index_rel , SpGistState *state , Point * queryPoint)
{
  // elog(NOTICE, "FindPoint -------------- start");
  // printf("\n------------------------------------FindPoint:\n\n");
  // create the first element
  stackItemData pageItem , *stackItem;
     
    // pgstat_count_index_scan(index);

    pageItem.blkno = SPGIST_ROOT_BLKNO;

    BlockIdSet(&(pageItem.ptr.ip_blkid), SPGIST_ROOT_BLKNO);
      
  pageItem.ptr.ip_posid = FirstOffsetNumber;
  pageItem.level = 1;
  pageItem.P_min.x = 0.0;
  pageItem.P_min.y = 0.0;

  // other initializations needed
  Buffer buffer = InvalidBuffer;
  List    *stack;
  // Snapshot snapshot;
  // DataBlock_info *dataBlock_arr[MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
  // init_dataBlock_arr(dataBlock_arr);
  // dataBlock *Block_arr[MAX_NO_LEAF_PAGE]; // list of the datablocks pointers
  // init_Block_arr();  
  
  
  stack = NIL;
  
  // first element in the stack (root)
  stack = lcons(&pageItem, stack);  

  //DEBUG
  int pageCnt = 0;

  //DEBUG

  // start iterating over the index tree 
  while (true )
  {
    BlockNumber blkno;
    OffsetNumber offset;
    Page    page;
    bool    isnull;
    
    /* Pull next to-do item from the list */
    if (stack == NIL)
      break;        /* there are no more pages to scan */

    CHECK_FOR_INTERRUPTS();

    stackItem = NULL;
    stackItem = (stackItemData*)linitial(stack);
    stack = list_delete_first(stack);
  
  redirect:  
  
    /* Check for interrupts, just in case of infinite loop */
    CHECK_FOR_INTERRUPTS();

    blkno = ItemPointerGetBlockNumber(&stackItem->ptr);
    offset = ItemPointerGetOffsetNumber(&stackItem->ptr);

     //DEBUG
    // if(blkno == 133)
    // printf("read = %d , hit = %d\n",index->pgstat_info->t_counts.t_blocks_fetched ,  index->pgstat_info->t_counts.t_blocks_hit);
    //DEBUG

    if (buffer == InvalidBuffer)
    {
      buffer = ReadBuffer(index_rel, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }
    else if (blkno != BufferGetBlockNumber(buffer))
    {
      UnlockReleaseBuffer(buffer);
      buffer = ReadBuffer(index_rel, blkno);
      LockBuffer(buffer, BUFFER_LOCK_SHARE);
    }

    //DEBUG
    // if(blkno == 133)
    // printf("read = %d , hit = %d\n",index->pgstat_info->t_counts.t_blocks_fetched ,  index->pgstat_info->t_counts.t_blocks_hit);
    
    //DEBUG
    

    page = BufferGetPage(buffer);

    isnull = SpGistPageStoresNulls(page) ? true : false;
    if (SpGistPageIsLeaf(page))
    {
      //DEBUG
      // elog(NOTICE, "FindPoint -------------- 2 , Leaf");
      pageCnt++;
      //DEBUg
      if (buffer != InvalidBuffer)
         UnlockReleaseBuffer(buffer);

      pfree(stackItem);
      stackItem = NULL;

      list_free_deep(stack);
      return blkno;
      //====================================================================
       
      //====================================================================
      
    }
    else /* Page is inner*/
    {
      // elog(NOTICE, "FindPoint ----------------- 3 , Inner ");
      SpGistInnerTuple innerTuple;
      // spgInnerConsistentIn in;
      // spgInnerConsistentOut out;
      int * arrOut;
      int outcnt =0 ;
      // FmgrInfo   *procinfo;
      SpGistNodeTuple *nodes;
      SpGistNodeTuple node;
      int     i;
      Point* centroid;
      // *LeafReached = false;

      innerTuple = (SpGistInnerTuple) PageGetItem(page,
                    PageGetItemId(page, offset));

      centroid = DatumGetPointP(SGITDATUM(innerTuple, state));
      if (innerTuple->tupstate != SPGIST_LIVE)
      {
        // DEBUG
        printf("\n\nLeaf tuple is NOT LIVE\n");
        //DEBUG
        if (innerTuple->tupstate == SPGIST_REDIRECT)
        {
          // DEBUG
          printf("Leaf tuple Redirect\n");
          //DEBUG

          /* transfer attention to redirect point */
          stackItem->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
          Assert(ItemPointerGetBlockNumber(&stackItem->ptr) != SPGIST_METAPAGE_BLKNO);
          goto redirect;
        }
        elog(ERROR, "unexpected SPGiST tuple state: %d",
           innerTuple->tupstate);
      }

      // elog(NOTICE, "FindPoint ----------------- 3 , Inner Consistent function");
      //=============================================================
      /* call Inner_consisten Fucntion */ 
      // ScanKey key = palloc(sizeof(ScanKey));
      // key->sk_strategy = RTSameStrategyNumber; 
      // key->sk_argument = PointPGetDatum(queryPoint);
      

      // in.scankeys = key;
      // in.nkeys = 1;
      // in.reconstructedValue = stackEntry->reconstructedValue;
      // in.traversalMemoryContext = oldCtx;
      // in.traversalValue = stackEntry->traversalValue;
      
      // in.level = stackItem->level;
      // in.returnData = so->want_itup;
      
      // in.allTheSame = innerTuple->allTheSame;
      // in.hasPrefix = (innerTuple->prefixSize > 0);
      // in.prefixDatum = SGITDATUM(innerTuple, state);
      // in.nNodes = innerTuple->nNodes;
      
      // in.nodeLabels = spgExtractNodeLabels(state, innerTuple);

      // elog(NOTICE, "FindPoint ----------------- 3.1 , inner Point : (%f,%f)  - queryPoint (%f,%f)" ,
      //                                                    DatumGetPointP(in.prefixDatum)->x,
      //                                                    DatumGetPointP(in.prefixDatum)->y,
      //                                                    queryPoint->x , queryPoint->y);
      // /* collect node pointers */
      nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * innerTuple->nNodes);
      SGITITERATE(innerTuple, i, node)
      {
        nodes[i] = node;
      }

      // elog(NOTICE, "FindPoint ----------------- 4");
      // memset(&out, 0, sizeof(out));

      if (!isnull)
      {
        /* use user-defined inner consistent method */
        // procinfo = index_getprocinfo(index_rel, 1, SPGIST_INNER_CONSISTENT_PROC);
        // FunctionCall2Coll(procinfo,
        //           index_rel->rd_indcollation[0],
        //           PointerGetDatum(&in),
        //           PointerGetDatum(&out));
        
        // my_spg_quad_inner_consistent(&in, &out);

        if (innerTuple->allTheSame)
        {
          /* Report that all nodes should be visited */
          // out->nNodes = innerTuple->nNodes;
          arrOut = (int *) palloc(sizeof(int) * innerTuple->nNodes);
          for (i = 0; i < innerTuple->nNodes; i++)
            arrOut[i] = i;
          
        }
        else
        {
          /* "which" is a bitmask of quadrants that satisfy all constraints */
          
          int which = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
          which &= (1 << getQuadrant(centroid, queryPoint));
          

          /* We must descend into the quadrant(s) identified by which */
          arrOut = (int *) palloc(sizeof(int) * 4);
          // out->nNodes = 0;
          // int i;
          outcnt = 0;
          for (i = 1; i <= 4; i++)
          {
            if (which & (1 << i))
              arrOut[outcnt++] = i - 1;
          }
        }
        // elog(NOTICE, "FindPoint ----------------- 5");
      }
      else
      {
        /* force all children to be visited */
        // out.nNodes = in.nNodes;
        arrOut = (int *) palloc(sizeof(int) * innerTuple->nNodes);
        for (i = 0; i < innerTuple->nNodes; i++)
          arrOut[i] = i;
        // elog(NOTICE, "FindPoint ----------------- 6");
      }

      /* If allTheSame, they should all or none of 'em match */
      // if (innerTuple->allTheSame)
      //   if (out.nNodes != 0 && out.nNodes != in.nNodes)
      //     elog(ERROR, "inconsistent inner_consistent results for allTheSame inner tuple");

      for (i = 0; i < outcnt; i++)
      {
        int     nodeN = arrOut[i];

        Assert(nodeN >= 0 && nodeN < innerTuple->nNodes);
        if (ItemPointerIsValid(&nodes[nodeN]->t_tid))
        {
          // elog(NOTICE, "FindPoint ----------------- 6.1");
          stackItemData *newEntry;

          /* Create new work item for this node */
          newEntry = palloc(sizeof(stackItemData));
          newEntry->ptr = nodes[nodeN]->t_tid;
          newEntry->blkno = ItemPointerGetBlockNumber(&nodes[nodeN]->t_tid);
          // elog(NOTICE, "FindPoint ----------------- 6.2");
          
          // if (out.levelAdds)
          //   newEntry->level = stackItem->level + out.levelAdds[i];
          // else
          //   newEntry->level = stackItem->level;
          
          stack = lcons(newEntry, stack);
          // elog(NOTICE, "FindPoint ----------------- 6.3");
          
        }
      }
      // elog(NOTICE, "FindPoint ----------------- 7");

      
      // pfree(innerTuple);
      // pfree(procinfo);
      // pfree(nodes);
      // pfree(stackItem);
    }
  }
  if (buffer != InvalidBuffer)
    UnlockReleaseBuffer(buffer);

  //DEBUG
  // printf("\n\nReadDataBlock: pageCnt = %d\n\n" , pageCnt);
  pfree(stackItem);
  stackItem = NULL;
  
  elog(NOTICE, "FindPoint ----------------- Finish ");
  return SPGIST_ROOT_BLKNO;
  //DEBUg
}



// typedef struct   // represeneted as a Page
// {
//   int level;
//   int which;
//   int blkno;
//   Point centroid;
//   bool isLeaf;
//   int count;
//   Point * points; // [MAX_NO_POINTS_BLOCK];
// }QuadData;

// typedef struct QuadNodeD
// {
//   Point min , max;

//   QuadData * data;

//   struct QuadNodeD * Quad1;
//   struct QuadNodeD * Quad2;
//   struct QuadNodeD * Quad3;
//   struct QuadNodeD * Quad4;

// }QuadNodeD;

// typedef struct QuadNodeD * QuadNode;

// void make_quadData( QuadData * node, int level, int which , int blkno , Point* centroid, bool isLeaf )
// {
//   node = palloc(sizeof (node));
//   node->level = level;
//   node->which = which;
//   node->blkno = blkno;
//   node->isLeaf = isLeaf;
//   node->centroid.x = centroid->x;
//   node->centroid.y = centroid->y;

//   node->count = 0;
// }

// void insert_quaddata_points(QuadData * node , Point * p)
// {
  
//   if(node->count+1 >= MAX_NO_POINTS_BLOCK)
//     elog(ERROR , "Quad Node is full , can't do insertion\n");

//   node->count++;
//   if(node->points == NULL )
//   {
//     node->points = palloc(sizeof(Point) * MAX_NO_POINTS_BLOCK);
//   }
  
//   node->points[node->count].x = p->x;
//   node->points[node->count].y = p->y;
// }

// void make_QuadNode( QuadNode root , Point * min, Point * max , Point * centroid , bool isLeaf , int blkno , int which , int level)
// {
//   root = palloc(sizeof(root));
//   root->min.x = min->x;
//   root->min.y = min->y;
//   root->max.x = max->x;
//   root->max.y = max->y;

//   make_quadData(root->data , level, which , blkno , centroid , isLeaf);

//   root->Quad1 = NULL;
//   root->Quad2 = NULL;
//   root->Quad3 = NULL;
//   root->Quad4 = NULL;

// }

// void insert_QuadNode( QuadNode root , QuadNode newnode)
// {
//   if(newnode == NULL) return;

//   if(root == NULL) return;

//   switch(newnode->data->which)
//   {
//     case 1: 
//       // if(newnode->data->isLeaf)
//       // root->Quad1 = 
//     break;
//     case 2: 
//     break;
//     case 3: 
//     break;
//     case 4: 
//     break;
//     case 0: /* root */ 
//     break;

//     default: 
//     break;
    
    
     
//   }

// }

//===========================================================
Datum
myspghandler(PG_FUNCTION_ARGS)
{
  IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

  
  amroutine->amstrategies = 0;
  amroutine->amsupport = SPGISTNProc;
  amroutine->amcanorder = false;
  amroutine->amcanorderbyop = true; // can order by 
  amroutine->amcanbackward = false;
  amroutine->amcanunique = false;
  amroutine->amcanmulticol = false;
  amroutine->amoptionalkey = true;
  amroutine->amsearcharray = false;
  amroutine->amsearchnulls = true;
  amroutine->amstorage = false;
  amroutine->amclusterable = false;
  amroutine->ampredlocks = false;
  amroutine->amkeytype = InvalidOid;

  amroutine->ambuild = spgbuild;
  amroutine->ambuildempty = spgbuildempty;
  amroutine->aminsert = spginsert;
  amroutine->ambulkdelete = spgbulkdelete;
  amroutine->amvacuumcleanup = spgvacuumcleanup;
  amroutine->amcanreturn = spgcanreturn;
  amroutine->amcostestimate = spgcostestimate;
  amroutine->amoptions = spgoptions;
  amroutine->amproperty = NULL;
  amroutine->amvalidate = spgvalidate;
  amroutine->ambeginscan = myspgbeginscan;
  amroutine->amrescan = myspgrescan;
  amroutine->amgettuple = myspggettuple;
  amroutine->amgetbitmap = spggetbitmap;
  amroutine->amendscan = spgendscan;
  amroutine->ammarkpos = NULL;
  amroutine->amrestrpos = NULL;

  PG_RETURN_POINTER(amroutine);
}



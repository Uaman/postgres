/*--------------------------------------------------------------------------
 * sahandler.c
 *	  Access method handler, validation, and cost estimation for the
 *	  suffix array index.
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/backend/access/saindex/sahandler.c
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/saindex_private.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *				AM handler
 * ----------------------------------------------------------------
 */

/*
 * sahandler
 *		Return an IndexAmRoutine populated with the SA index callbacks.
 *
 * This is the entry point registered in pg_am.amhandler.
 */
Datum
sahandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	/* ---- AM properties ---- */
	amroutine->amstrategies = 3;			/* contains, prefix, suffix */
	amroutine->amsupport = SA_COMPARE_PROC; /* 1 support proc */
	amroutine->amoptsprocnum = 0;			/* no options proc */
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;		/* single text column only */
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;			/* key type == input type */
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = true;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	/* ---- AM callback functions ---- */

	/* Build */
	amroutine->ambuild = sabuild;
	amroutine->ambuildempty = sabuildempty;

	/* Insert (not supported yet) */
	amroutine->aminsert = NULL;
	amroutine->aminsertcleanup = NULL;

	/* Vacuum: ambulkdelete not yet supported; vacuumcleanup is a no-op
	 * required so ANALYZE (which calls index_vacuum_cleanup) doesn't error. */
	amroutine->ambulkdelete = NULL;
	amroutine->amvacuumcleanup = savacuumcleanup;

	/* Index return */
	amroutine->amcanreturn = NULL;			/* no index-only scans */

	/* Cost estimation */
	amroutine->amcostestimate = sacostestimate;

	/* Misc */
	amroutine->amgettreeheight = NULL;
	amroutine->amoptions = saoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = sabuildphasename;
	amroutine->amvalidate = savalidate;
	amroutine->amadjustmembers = NULL;

	/* Scan */
	amroutine->ambeginscan = sabeginscan;
	amroutine->amrescan = sarescan;
	amroutine->amgettuple = NULL;			/* bitmap scans only */
	amroutine->amgetbitmap = sagetbitmap;
	amroutine->amendscan = saendscan;

	/* Position */
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;

	/* Parallel (not supported) */
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}


/* ----------------------------------------------------------------
 *				Validation
 * ----------------------------------------------------------------
 */

/*
 * savalidate
 *		Minimal opclass validator for the SA index.
 *
 * For now, simply returns true.  A full implementation would check that
 * the opclass provides the expected operators (strategies 1-3) and
 * support procedures (proc 1).
 */
bool
savalidate(Oid opclassoid)
{
	return true;
}


/* ----------------------------------------------------------------
 *				Vacuum
 * ----------------------------------------------------------------
 */

/*
 * savacuumcleanup
 *		Post-vacuum / ANALYZE callback for the SA index.
 *
 * Called by index_vacuum_cleanup(), including from the ANALYZE path
 * (info->analyze_only == true).  Since the suffix-array image is fully
 * rebuilt during sabuild() and we do not support incremental deletes yet,
 * there is nothing to do here except return the existing stats struct.
 */
IndexBulkDeleteResult *
savacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* Nothing to do — the SA page is immutable until the next REINDEX. */
	return stats;
}


/* ----------------------------------------------------------------
 *				Cost estimation
 * ----------------------------------------------------------------
 */

/*
 * sacostestimate
 *		Cost estimation for SA index scans.
 *
 * Uses genericcostestimate as a baseline and then adjusts for
 * the SA index's search characteristics:
 *   - Binary search: O(log N) random page reads
 *   - Sequential scan of auxiliary arrays for the matching range
 *
 * This is a simplified estimator suitable for a research prototype.
 */
void
sacostestimate(PlannerInfo *root, IndexPath *path,
			   double loop_count,
			   Cost *indexStartupCost,
			   Cost *indexTotalCost,
			   Selectivity *indexSelectivity,
			   double *indexCorrelation,
			   double *indexPages)
{
	GenericCosts costs;

	MemSet(&costs, 0, sizeof(costs));

	genericcostestimate(root, path, loop_count, &costs);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

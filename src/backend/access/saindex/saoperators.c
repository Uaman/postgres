/*--------------------------------------------------------------------------
 * saoperators.c
 *	  Operator implementation functions for the suffix array index.
 *
 *	  These are SQL-callable boolean functions that implement the three
 *	  SA index operators.  They are used by the executor for:
 *	  - evaluating the operator in non-index contexts (e.g., SeqScan)
 *	  - heap rechecks when the index returns lossy results
 *
 *	  Additionally provides the support comparison function used
 *	  by the SA opclass for internal key ordering.
 *
 * Copyright (c) 2025, Dmytro Zvazhii
 *
 * src/backend/access/saindex/saoperators.c
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/saindex_private.h"
#include "utils/builtins.h"
#include "utils/varlena.h"


/* ----------------------------------------------------------------
 *				Operator functions
 * ----------------------------------------------------------------
 */

/*
 * sa_text_contains
 *		Returns true if text1 contains text2 as a substring.
 *
 * Implements the @> (text, text) operator — strategy 1 (CONTAINS).
 * Corresponds to: WHERE col @> 'pattern'  (i.e., LIKE '%pattern%')
 *
 * Uses byte-wise comparison (not collation-aware), matching the
 * SA index's memcmp-based ordering.
 */
Datum
sa_text_contains(PG_FUNCTION_ARGS)
{
	text	   *haystack = PG_GETARG_TEXT_PP(0);
	text	   *needle = PG_GETARG_TEXT_PP(1);
	int			hlen = VARSIZE_ANY_EXHDR(haystack);
	int			nlen = VARSIZE_ANY_EXHDR(needle);
	char	   *hdata = VARDATA_ANY(haystack);
	char	   *ndata = VARDATA_ANY(needle);
	bool		found = false;
	int			i;

	if (nlen == 0)
	{
		/* Empty pattern is contained in every string */
		PG_RETURN_BOOL(true);
	}

	if (nlen > hlen)
	{
		PG_RETURN_BOOL(false);
	}

	/*
	 * Simple byte-wise substring search.  For a research prototype this is
	 * adequate.  A production implementation might use a more efficient
	 * algorithm (e.g., Boyer-Moore or KMP).
	 */
	for (i = 0; i <= hlen - nlen; i++)
	{
		if (memcmp(hdata + i, ndata, nlen) == 0)
		{
			found = true;
			break;
		}
	}

	PG_RETURN_BOOL(found);
}

/*
 * sa_text_prefix
 *		Returns true if text1 starts with text2.
 *
 * Implements the ^@ (text, text) operator — strategy 2 (PREFIX).
 * Corresponds to: WHERE col ^@ 'pattern'  (i.e., LIKE 'pattern%')
 *
 * Note: PostgreSQL already has starts_with() for the ^@ operator,
 * but we define our own for SA opclass consistency.  The existing
 * ^@ operator (OID 3877) is reused in the opclass registration.
 */
Datum
sa_text_prefix(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *prefix = PG_GETARG_TEXT_PP(1);
	int			slen = VARSIZE_ANY_EXHDR(str);
	int			plen = VARSIZE_ANY_EXHDR(prefix);

	if (plen > slen)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(memcmp(VARDATA_ANY(str), VARDATA_ANY(prefix), plen) == 0);
}

/*
 * sa_text_suffix
 *		Returns true if text1 ends with text2.
 *
 * Implements the $@ (text, text) operator — strategy 3 (SUFFIX).
 * Corresponds to: WHERE col $@ 'pattern'  (i.e., LIKE '%pattern')
 */
Datum
sa_text_suffix(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *suffix = PG_GETARG_TEXT_PP(1);
	int			slen = VARSIZE_ANY_EXHDR(str);
	int			xlen = VARSIZE_ANY_EXHDR(suffix);

	if (xlen > slen)
		PG_RETURN_BOOL(false);

	PG_RETURN_BOOL(memcmp(VARDATA_ANY(str) + slen - xlen,
						   VARDATA_ANY(suffix), xlen) == 0);
}


/* ----------------------------------------------------------------
 *				Support function
 * ----------------------------------------------------------------
 */

/*
 * sa_text_cmp
 *		Byte-wise comparison of two text values.
 *
 * Support procedure 1 (SA_COMPARE_PROC) for the SA opclass.
 * Returns int4: negative if a < b, 0 if equal, positive if a > b.
 *
 * Uses memcmp ordering, consistent with the SA index's sort order.
 */
Datum
sa_text_cmp(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);
	int			alen = VARSIZE_ANY_EXHDR(a);
	int			blen = VARSIZE_ANY_EXHDR(b);
	int			cmplen = Min(alen, blen);
	int			result;

	result = memcmp(VARDATA_ANY(a), VARDATA_ANY(b), cmplen);

	if (result == 0)
	{
		if (alen < blen)
			result = -1;
		else if (alen > blen)
			result = 1;
	}

	PG_RETURN_INT32(result);
}

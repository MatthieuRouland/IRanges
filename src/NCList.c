/****************************************************************************
 *                 A Nested Containment List implementation                 *
 *                                                                          *
 *                           Author: Herve Pages                            *
 ****************************************************************************/
#include "IRanges.h"
#include "S4Vectors_interface.h"

#include <stdlib.h>  /* for malloc, realloc, free, qsort */
#include <math.h>    /* for log10 */

/*
#include <time.h>
static clock_t clock0;
static void init_clock(const char *msg)
{
	printf("%s", msg);
	clock0 = clock();
}
static void print_elapsed_time()
{
	printf("%8.6f s\n", ((double) clock() - clock0) / CLOCKS_PER_SEC);
}
*/

typedef struct pnclist {
	int buflength;           /* always >= 0 */
	int nelt;                /* always >= 0 and <= buflength */
	struct pnclist_elt *elts;
} preNCList;

typedef struct pnclist_elt {
	int n2x;		/* 0-based back index into 'x' (Ranges) */
	struct pnclist nested_list;
} preNCListElt;


static void init_preNCList(preNCList *pnclist)
{
	pnclist->buflength = pnclist->nelt = 0;
	return;
}

static void init_preNCListElt(preNCListElt *elt, int n2x)
{
	elt->n2x = n2x;
	init_preNCList(&(elt->nested_list));
	return;
}

static void free_preNCList(const preNCList *pnclist)
{
	int n;
	const preNCListElt *elt;

	if (pnclist->buflength == 0)
		return;
	for (n = 0, elt = pnclist->elts; n < pnclist->nelt; n++, elt++)
		free_preNCList(&(elt->nested_list));
	free(pnclist->elts);
	return;
}


/****************************************************************************
 * preNCList_new() and preNCList_free()
 */

/* --- .Call ENTRY POINT --- */
SEXP preNCList_new()
{
	preNCList *top_pnclist;

	//init_clock("preprocessing: T1 = ");
	top_pnclist = (preNCList *) malloc(sizeof(preNCList));
	if (top_pnclist == NULL)
		error("preNCList_new: memory allocation failed");
	init_preNCList(top_pnclist);
	return R_MakeExternalPtr(top_pnclist, R_NilValue, R_NilValue);
}

/* --- .Call ENTRY POINT --- */
SEXP preNCList_free(SEXP pnclist)
{
	preNCList *top_pnclist;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("preNCList_free: pointer to preNCList struct is NULL");
	free_preNCList(top_pnclist);
	free(top_pnclist);
	R_SetExternalPtrAddr(pnclist, NULL);
	return R_NilValue;
}


/****************************************************************************
 * preNCList_build()
 */

static void extend_preNCList(preNCList *pnclist)
{
	int old_buflength, new_buflength;
	size_t elt_size;
	preNCListElt *new_elts;

	old_buflength = pnclist->buflength;
	elt_size = sizeof(preNCListElt);
	if (old_buflength == 0) {
		new_buflength = 4;
		new_elts = (preNCListElt *) malloc(elt_size * new_buflength);
	} else {
		if (old_buflength < 16384)
			new_buflength = 8 * old_buflength;
		else if (old_buflength < 4194304)
			new_buflength = 4 * old_buflength;
		else if (old_buflength < 67108864)
			new_buflength = 2 * old_buflength;
		else
			new_buflength = old_buflength + 33554432;
		new_elts = (preNCListElt *) realloc(pnclist->elts,
						    new_buflength * elt_size);
	}
	if (new_elts == NULL)
		error("extend_preNCList: memory allocation failed");
	pnclist->buflength = new_buflength;
	pnclist->elts = new_elts;
	return;
}

static preNCListElt *append_preNCList_elt(preNCList *host, int n2x)
{
	preNCListElt *new_elt;

	if (host->nelt == host->buflength)
		extend_preNCList(host);
	new_elt = host->elts + host->nelt;
	init_preNCListElt(new_elt, n2x);
	host->nelt++;
	return new_elt;
}

static const int *aa, *bb;

static int qsort_compar(const void *p1, const void *p2)
{
	int i1, i2, ret;

	i1 = *((const int *) p1);
	i2 = *((const int *) p2);
	ret = aa[i1] - aa[i2];
	if (ret != 0)
		return ret;
	ret = bb[i2] - bb[i1];
	return ret;
}

/*
 * Setting a hard limit on the max depth of NCList objects to prevent C stack
 * overflows when running recursive code like nclist_get_y_overlaps().
 * A better solution would be to not use recursive code at all when traversing
 * an NCList object. Then NCList objects of arbitrary depth could be supported
 * and it wouldn't be necessary to set the limit below.
 */
#define NCLIST_MAX_DEPTH 25000
static preNCListElt **stack = NULL;
static int stack_length = 0;

static void extend_stack()
{
	int new_length;
	preNCListElt **new_stack;

	if (stack_length == 0) {
		new_length = 1000;
		new_stack = (preNCListElt **) malloc(sizeof(preNCListElt *) *
						     new_length);
	} else {
		if (stack_length == NCLIST_MAX_DEPTH)
			error("extend_stack: cannot build an NCList object "
			      "of depth >= %d", NCLIST_MAX_DEPTH);
		if (stack_length <= NCLIST_MAX_DEPTH / 2)
			new_length = 2 * stack_length;
		else
			new_length = NCLIST_MAX_DEPTH;
		new_stack = (preNCListElt **) realloc(stack,
						      sizeof(preNCListElt *) *
						      new_length);
	}
	if (new_stack == NULL)
		error("extend_stack: memory allocation failed");
	stack_length = new_length;
	stack = new_stack;
	return;
}

static void build_preNCList(preNCList *top_pnclist,
			    const int *x_start_p, const int *x_end_p,
			    const int *x_subset_p, int x_len)
{
	int *oo, k, d, i, current_end;
	preNCList *host;
	preNCListElt *new_elt;

	/* Determine order of 'x'. 'oo' will be such that 'x[oo]' is sorted
	   first by ascending start then by descending end. */
	oo = (int *) R_alloc(sizeof(int), x_len);
	if (x_subset_p == NULL)
		for (i = 0; i < x_len; i++)
			oo[i] = i;
	else
		for (i = 0; i < x_len; i++)
			oo[i] = x_subset_p[i];
	aa = x_start_p;
	bb = x_end_p;
	qsort(oo, x_len, sizeof(int), qsort_compar);

	init_preNCList(top_pnclist);
	for (k = 0, d = -1; k < x_len; k++) {
		i = oo[k];
		current_end = x_end_p[i];
		while (d >= 0 && x_end_p[stack[d]->n2x] < current_end)
			d--;  // unstack
		host = d == -1 ? top_pnclist: &(stack[d]->nested_list);
		// append range i to host
		new_elt = append_preNCList_elt(host, i);
		if (++d == stack_length)
			extend_stack();
		stack[d] = new_elt;  // stack
	}
	return;
}

/* --- .Call ENTRY POINT --- */
SEXP preNCList_build(SEXP pnclist, SEXP x_start, SEXP x_end, SEXP x_subset)
{
	preNCList *top_pnclist;
	int x_len;
	const int *x_start_p, *x_end_p, *x_subset_p;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("preNCList_build: pointer to preNCList struct is NULL");
	x_len = check_integer_pairs(x_start, x_end,
				    &x_start_p, &x_end_p,
				    "start(x)", "end(x)");
	if (x_subset == R_NilValue) {
		x_subset_p = NULL;
	} else {
		x_subset_p = INTEGER(x_subset);
		x_len = LENGTH(x_subset);
	}
	build_preNCList(top_pnclist, x_start_p, x_end_p, x_subset_p, x_len);
	return pnclist;
}


/****************************************************************************
 * new_NCList_from_preNCList()
 */

#define NCLIST_NELT(nclist) ((nclist)[0])
#define NCLIST_N2X(nclist, n) ((nclist)[((n)<<1)+1])
#define NCSUBLIST_OFFSET(nclist, n) ((nclist)[((n)<<1)+2])

static int compute_length_of_preNCList_as_INTEGER(const preNCList *pnclist)
{
	int nelt, n;
	unsigned int ans_len, dump_len;
	const preNCListElt *elt;

	nelt = pnclist->nelt;
	if (nelt == 0)
		return 0;
	ans_len = 1U + 2U * (unsigned int) nelt;
	for (n = 0, elt = pnclist->elts; n < nelt; n++, elt++) {
		dump_len = compute_length_of_preNCList_as_INTEGER(
					&(elt->nested_list));
		ans_len += dump_len;
		if (ans_len < dump_len)
			goto too_big;
	}
	if (ans_len <= INT_MAX)
		return (int) ans_len;
too_big:
	error("compute_length_of_preNCList_as_INTEGER: "
	      "preNCList object is too big to fit in an integer vector");
}

static int dump_preNCList_as_int_array(const preNCList *pnclist, int *out)
{
	int nelt, offset, dump_len, n;
	const preNCListElt *elt;

	nelt = pnclist->nelt;
	if (nelt == 0)
		return 0;
	offset = 1 + 2 * nelt;
	NCLIST_NELT(out) = nelt;
	for (n = 0, elt = pnclist->elts; n < nelt; n++, elt++) {
		NCLIST_N2X(out, n) = elt->n2x;
		dump_len = dump_preNCList_as_int_array(&(elt->nested_list),
						       out + offset);
		NCSUBLIST_OFFSET(out, n) = dump_len != 0 ? offset : -1;
		offset += dump_len;
	}
	return offset;
}

/* --- .Call ENTRY POINT --- */
SEXP new_NCList_from_preNCList(SEXP pnclist)
{
	SEXP ans;
	const preNCList *top_pnclist;
	int ans_len;

	top_pnclist = (preNCList *) R_ExternalPtrAddr(pnclist);
	if (top_pnclist == NULL)
		error("new_NCList_from_preNCList: "
		      "pointer to preNCList struct is NULL");
	ans_len = compute_length_of_preNCList_as_INTEGER(top_pnclist);
	PROTECT(ans = NEW_INTEGER(ans_len));
	dump_preNCList_as_int_array(top_pnclist, INTEGER(ans));
	UNPROTECT(1);
	//print_elapsed_time();
	return ans;
}


/****************************************************************************
 * NCList_print()
 */

/* Print 1 line per range in 'nclist'. Return max depth. */
static int print_NCList(const int *nclist,
			const int *x_start_p, const int *x_end_p,
			int depth, const char *format)
{
	int max_depth, nelt, n, d, n2x, offset, tmp;

	max_depth = depth;
	nelt = NCLIST_NELT(nclist);
	for (n = 0; n < nelt; n++) {
		for (d = 1; d < depth; d++)
			Rprintf("|");
		n2x = NCLIST_N2X(nclist, n);
		Rprintf(format, n2x + 1);
		Rprintf(": [%d, %d]\n", x_start_p[n2x], x_end_p[n2x]);
		offset = NCSUBLIST_OFFSET(nclist, n);
		if (offset != -1) {
			tmp = print_NCList(nclist + offset,
					   x_start_p, x_end_p, depth + 1,
					   format);
			if (tmp > max_depth)
				max_depth = tmp;
		}
	}
	return max_depth;
}

/* --- .Call ENTRY POINT --- */
SEXP NCList_print(SEXP x_nclist, SEXP x_start, SEXP x_end)
{
	const int *top_nclist;
	int x_len, max_digits, max_depth;
	const int *x_start_p, *x_end_p;
	char format[10];

	top_nclist = INTEGER(x_nclist);
	x_len = check_integer_pairs(x_start, x_end,
				    &x_start_p, &x_end_p,
				    "start(x)", "end(x)");
	if (x_len == 0) {
		max_depth = 0;
	} else {
		max_digits = (int) log10((double) x_len) + 1;
		sprintf(format, "%c0%d%c", '%', max_digits, 'd');
		max_depth = print_NCList(top_nclist, x_start_p, x_end_p,
					 1, format);
	}
	Rprintf("max depth = %d\n", max_depth);
	return R_NilValue;
}


/****************************************************************************
 * pp_find_overlaps()
 */

/* 6 supported types of overlap. */
#define TYPE_ANY		1
#define TYPE_START		2
#define TYPE_END		3
#define TYPE_WITHIN		4
#define TYPE_EXTEND		5
#define TYPE_EQUAL		6

typedef struct backpack {
	/* Members set by prepare_backpack(). */
	const int *x_start_p;
	const int *x_end_p;
	const int *x_space_p;
	int min_overlap_score0;
	int y_extension;
	int overlap_type;
	int select_mode;
	int circle_len;
	int pp_is_q;
	IntAE *hits;
	int *direct_out;

	/* Members set by update_backpack(). */
	int y_start;
	int y_end;
	int y_space;
	int ext_y_start;
	int ext_y_end;
	int y_idx;
} Backpack;

static int is_hit(int x_idx, const Backpack *backpack)
{
	static int x_space, x_start, x_end, ov_start, ov_end, d;

	/* Check the space */
	if (backpack->x_space_p != NULL && backpack->y_space != 0) {
		x_space = backpack->x_space_p[x_idx];
		if (x_space != 0 && x_space != backpack->y_space)
			return 0;
	}
	/* Check the score */
	x_start = backpack->x_start_p[x_idx];
	x_end = backpack->x_end_p[x_idx];
	ov_start = backpack->y_start > x_start ?
		   backpack->y_start : x_start;
	ov_end   = backpack->y_end < x_end ?
		   backpack->y_end : x_end;
	if (ov_end - ov_start < backpack->min_overlap_score0)
		return 0;
	/* Check the type */
	if (backpack->overlap_type == TYPE_ANY)
		return 1;
	if (backpack->overlap_type == TYPE_WITHIN)
		return backpack->y_start >= x_start &&
		       backpack->y_end <= x_end;
	if (backpack->overlap_type == TYPE_EXTEND)
		return backpack->y_start <= x_start &&
		       backpack->y_end >= x_end;
	if (backpack->overlap_type == TYPE_START)
		return backpack->y_start == x_start;
	if (backpack->overlap_type == TYPE_END) {
		d = backpack->y_end - x_end;
		if (backpack->circle_len != NA_INTEGER)
			d %= backpack->circle_len;
		return d == 0;
	}
	/* TYPE_EQUAL */
	return backpack->y_start == x_start &&
	       backpack->y_end == x_end;
}

static void report_hit(int x_idx, const Backpack *backpack)
{
	int i1, q_idx, s_idx1, *selection_p;

	i1 = x_idx + 1;  /* 1-based */
	if (backpack->select_mode == ALL_HITS) {
		/* Report the hit. */
		IntAE_insert_at(backpack->hits,
				IntAE_get_nelt(backpack->hits), i1);
		return;
	}
	/* Update current selection if necessary. */
	if (backpack->pp_is_q) {
		q_idx = x_idx;
		s_idx1 = backpack->y_idx + 1;
	} else {
		q_idx = backpack->y_idx;
		s_idx1 = i1;
	}
	selection_p = backpack->direct_out + q_idx;
	if (backpack->select_mode == COUNT_HITS) {
		(*selection_p)++;
		return;
	}
	if (*selection_p == NA_INTEGER
	 || (backpack->select_mode == FIRST_HIT) ==
	    (s_idx1 < *selection_p))
		*selection_p = s_idx1;
	return;
}

static Backpack prepare_backpack(const int *x_start_p, const int *x_end_p,
				 const int *x_space_p, 
				 int min_overlap_score, int overlap_type,
				 int select_mode, int circle_len, int pp_is_q,
				 IntAE *hits, int *direct_out)
{
	Backpack backpack;
	int y_extension;

	y_extension = min_overlap_score >= 1 ? 0 : 1 - min_overlap_score;
	backpack.x_start_p = x_start_p;
	backpack.x_end_p = x_end_p;
	backpack.x_space_p = x_space_p;
	backpack.min_overlap_score0 = min_overlap_score - 1;
	backpack.y_extension = y_extension;
	backpack.overlap_type = overlap_type;
	backpack.select_mode = select_mode;
	backpack.circle_len = circle_len;
	backpack.pp_is_q = pp_is_q;
	backpack.hits = hits;
	backpack.direct_out = direct_out;
	return backpack;
}

static void update_backpack(Backpack *backpack, int y_start, int y_end,
						int y_space, int y_idx)
{
	backpack->y_start = y_start;
	backpack->y_end = y_end;
	backpack->y_space = y_space;
	backpack->ext_y_start = y_start - backpack->y_extension;
	backpack->ext_y_end = y_end + backpack->y_extension;
	backpack->y_idx = y_idx;
	return;
}

static void shift_y(Backpack *backpack, int shift)
{
	backpack->y_start += shift;
	backpack->y_end += shift;
	backpack->ext_y_start += shift;
	backpack->ext_y_end += shift;
	return;
}

/* TODO: Maybe move this to S4Vectors/src/AEbufs.c */
static void IntAE_delete_duplicates(IntAE *int_ae, int at1, int at2)
{
	int d, k0, k, val;

	d = at2 - at1;
	if (d <= 1)
		return;
	if (d >= 3)
		sort_int_array(int_ae->elts + at1, d, 0);
	k0 = at1;
	for (k = k0 + 1; k < at2; k++) {
		val = int_ae->elts[k];
		if (val == int_ae->elts[k0])
			continue;
		k0++;
		int_ae->elts[k0] = val;
	}
	IntAE_set_nelt(int_ae, k0 + 1);
	return;
}

typedef void (*GetYOverlapsFunType)(const void *, const Backpack *);

static void pp_find_overlaps(
		const int *q_start_p, const int *q_end_p,
		const int *q_space_p, const int *q_subset_p, int q_len,
		const int *s_start_p, const int *s_end_p,
		const int *s_space_p, const int *s_subset_p, int s_len,
		int min_overlap_score, int overlap_type,
		int select_mode, int circle_len,
		const void *pp, int pp_is_q,
		GetYOverlapsFunType get_y_overlaps,
		IntAE *qh_buf, IntAE *sh_buf, int *direct_out)
{
	const int *x_start_p, *x_end_p, *x_space_p,
		  *y_start_p, *y_end_p, *y_space_p, *y_subset_p;
	int y_len, backpack_select_mode, i, j, old_nhit, new_nhit, k;
	IntAE *xh_buf, *yh_buf;
	Backpack backpack;

	if (q_len == 0 || s_len == 0)
		return;
	if (pp_is_q) {
		x_start_p = q_start_p;
		x_end_p = q_end_p;
		x_space_p = q_space_p;
		xh_buf = qh_buf;
		y_start_p = s_start_p;
		y_end_p = s_end_p;
		y_space_p = s_space_p;
		y_subset_p = s_subset_p;
		y_len = s_len;
		yh_buf = sh_buf;
		if (overlap_type == TYPE_WITHIN)
			overlap_type = TYPE_EXTEND;
		else if (overlap_type == TYPE_EXTEND)
			overlap_type = TYPE_WITHIN;
	} else {
		x_start_p = s_start_p;
		x_end_p = s_end_p;
		x_space_p = s_space_p;
		xh_buf = sh_buf;
		y_start_p = q_start_p;
		y_end_p = q_end_p;
		y_space_p = q_space_p;
		y_subset_p = q_subset_p;
		y_len = q_len;
		yh_buf = qh_buf;
	}
	if (circle_len != NA_INTEGER && select_mode == COUNT_HITS)
		backpack_select_mode = ALL_HITS;
	else
		backpack_select_mode = select_mode;
	backpack = prepare_backpack(x_start_p, x_end_p, x_space_p,
				    min_overlap_score, overlap_type,
				    backpack_select_mode, circle_len, pp_is_q,
				    xh_buf, direct_out);
	for (i = 0; i < y_len; i++) {
		j = y_subset_p == NULL ? i : y_subset_p[i];
		update_backpack(&backpack, y_start_p[j], y_end_p[j],
				y_space_p == NULL ? 0 : y_space_p[j], j);
		/* pass 0 */
		get_y_overlaps(pp, &backpack);
		if (circle_len == NA_INTEGER)
			goto life_is_good;
		if (select_mode == ARBITRARY_HIT
		 && !pp_is_q && direct_out[j] != NA_INTEGER)
			goto life_is_good;
		/* pass 1 */
		shift_y(&backpack, - circle_len);
		get_y_overlaps(pp, &backpack);
		if (select_mode == ARBITRARY_HIT
		 && !pp_is_q && direct_out[j] != NA_INTEGER)
			goto life_is_good;
		/* pass 2 */
		shift_y(&backpack, 2 * circle_len);
		get_y_overlaps(pp, &backpack);

		life_is_good:
		if (backpack_select_mode != ALL_HITS)
			continue;
		old_nhit = IntAE_get_nelt(yh_buf);
		new_nhit = IntAE_get_nelt(xh_buf);
		if (circle_len != NA_INTEGER) {
			IntAE_delete_duplicates(xh_buf, old_nhit, new_nhit);
			new_nhit = IntAE_get_nelt(xh_buf);
		}
		if (select_mode != COUNT_HITS) {
			j++;  /* 1-based */
			for (k = old_nhit; k < new_nhit; k++)
				IntAE_insert_at(yh_buf, k, j);
			continue;
		}
		if (pp_is_q) {
			for (k = old_nhit; k < new_nhit; k++)
				direct_out[xh_buf->elts[k] - 1]++;
		} else {
			direct_out[j] += new_nhit - old_nhit;
		}
		IntAE_set_nelt(xh_buf, old_nhit);
	}
	return;
}


/****************************************************************************
 * nclist_get_y_overlaps()
 */

static int nclist_bsearch(const int *x_nclist, const int *x_end_p,
			  int y_start)
{
	int n1, n2, nelt, n, x_end;

	/* Check first element. */
	n1 = 0;
	x_end = x_end_p[NCLIST_N2X(x_nclist, n1)];
	if (x_end >= y_start)
		return n1;

	/* Check last element. */
	nelt = NCLIST_NELT(x_nclist);
	n2 = nelt - 1;
	x_end = x_end_p[NCLIST_N2X(x_nclist, n2)];
	if (x_end < y_start)
		return nelt;
	if (x_end == y_start)
		return n2;

	/* Binary search. */
	while ((n = (n1 + n2) / 2) != n1) {
		x_end = x_end_p[NCLIST_N2X(x_nclist, n)];
		if (x_end == y_start)
			return n;
		if (x_end < y_start)
			n1 = n;
		else
			n2 = n;
	}
	return n2;
}

/* Recursive! */
static void nclist_get_y_overlaps(const int *x_nclist, const Backpack *backpack)
{
	int nelt, n, n2x, offset;

	nelt = NCLIST_NELT(x_nclist);
	n = nclist_bsearch(x_nclist, backpack->x_end_p,
				     backpack->ext_y_start);
	for ( ; n < nelt; n++) {
		n2x = NCLIST_N2X(x_nclist, n);
		if (backpack->x_start_p[n2x] > backpack->ext_y_end)
			break;
		if (is_hit(n2x, backpack)) {
			report_hit(n2x, backpack);
			if (backpack->select_mode == ARBITRARY_HIT
			 && !backpack->pp_is_q)
				break;
		}
		offset = NCSUBLIST_OFFSET(x_nclist, n);
		if (offset != -1)
			nclist_get_y_overlaps(x_nclist + offset, backpack);
	}
	return;
}


/****************************************************************************
 * pnclist_get_y_overlaps()
 */

static int pnclist_bsearch(const preNCList *x_pnclist, const int *x_end_p,
			   int y_start)
{
	int n1, n2, nelt, n, x_end;

	/* Check first element. */
	n1 = 0;
	x_end = x_end_p[x_pnclist->elts[n1].n2x];
	if (x_end >= y_start)
		return n1;

	/* Check last element. */
	nelt = x_pnclist->nelt;
	n2 = nelt - 1;
	x_end = x_end_p[x_pnclist->elts[n2].n2x];
	if (x_end < y_start)
		return nelt;
	if (x_end == y_start)
		return n2;

	/* Binary search. */
	while ((n = (n1 + n2) / 2) != n1) {
		x_end = x_end_p[x_pnclist->elts[n].n2x];
		if (x_end == y_start)
			return n;
		if (x_end < y_start)
			n1 = n;
		else
			n2 = n;
	}
	return n2;
}

/* Recursive! */
static void pnclist_get_y_overlaps(const preNCList *x_pnclist,
				   const Backpack *backpack)
{
	int nelt, n, n2x;
	const preNCListElt *elt;

	nelt = x_pnclist->nelt;
	n = pnclist_bsearch(x_pnclist, backpack->x_end_p,
				       backpack->ext_y_start);
	for (elt = x_pnclist->elts + n; n < nelt; n++, elt++) {
		n2x = elt->n2x;
		if (backpack->x_start_p[n2x] > backpack->ext_y_end)
			break;
		if (is_hit(n2x, backpack)) {
			report_hit(n2x, backpack);
			if (backpack->select_mode == ARBITRARY_HIT
			 && !backpack->pp_is_q)
				break;
		}
		if (elt->nested_list.nelt != 0)
			pnclist_get_y_overlaps(&(elt->nested_list), backpack);
	}
	return;
}


/****************************************************************************
 * find_overlaps()
 */

static int find_overlaps(
		const int *q_start_p, const int *q_end_p,
		const int *q_space_p, const int *q_subset_p, int q_len,
		const int *s_start_p, const int *s_end_p,
		const int *s_space_p, const int *s_subset_p, int s_len,
		int min_overlap_score, int overlap_type,
		int select_mode, int circle_len,
		SEXP nclist, int pp_is_q,
		IntAE *qh_buf, IntAE *sh_buf, int *direct_out)
{
	preNCList pnclist;
	const void *pp;
	GetYOverlapsFunType get_y_overlaps;

	if (q_len == 0 || s_len == 0)
		return 0;
	if (nclist == R_NilValue) {
		/* On-the-fly preprocessing. */
		pp_is_q = q_len < s_len;
		if (pp_is_q)
			build_preNCList(&pnclist, q_start_p, q_end_p,
						  q_subset_p, q_len);
		else 
			build_preNCList(&pnclist, s_start_p, s_end_p,
						  s_subset_p, s_len);
		pp = &pnclist;
		get_y_overlaps = (GetYOverlapsFunType) pnclist_get_y_overlaps;
	} else {
		pp = INTEGER(nclist);
		get_y_overlaps = (GetYOverlapsFunType) nclist_get_y_overlaps;
	}
	pp_find_overlaps(
		q_start_p, q_end_p, q_space_p, q_subset_p, q_len,
		s_start_p, s_end_p, s_space_p, s_subset_p, s_len,
		min_overlap_score, overlap_type,
		select_mode, circle_len,
		pp, pp_is_q, get_y_overlaps,
		qh_buf, sh_buf, direct_out);
	if (nclist == R_NilValue)
		free_preNCList(&pnclist);
	return pp_is_q;
}


/****************************************************************************
 * Check user-supplied input.
 */

static int get_min_overlap_score(SEXP min_score)
{
	int min_overlap_score;

	if (!IS_INTEGER(min_score) || LENGTH(min_score) != 1)
		error("'min_score' must be a single integer");
	min_overlap_score = INTEGER(min_score)[0];
	if (min_overlap_score == NA_INTEGER)
		error("'min_score' cannot be NA");
	return min_overlap_score;
}

static int get_overlap_type(SEXP type)
{
	const char *type0;

	if (!IS_CHARACTER(type) || LENGTH(type) != 1)
		error("'type' must be a single string");
	type = STRING_ELT(type, 0);
	if (type == NA_STRING)
		error("'type' cannot be NA");
	type0 = CHAR(type);
	if (strcmp(type0, "any") == 0)
		return TYPE_ANY;
	if (strcmp(type0, "start") == 0)
		return TYPE_START;
	if (strcmp(type0, "end") == 0)
		return TYPE_END;
	if (strcmp(type0, "within") == 0)
		return TYPE_WITHIN;
	if (strcmp(type0, "extend") == 0)
		return TYPE_EXTEND;
	if (strcmp(type0, "equal") == 0)
		return TYPE_EQUAL;
	error("'type' must be \"any\", \"start\", \"end\", "
	      "\"within\", \"extend\", or \"equal\"");
	return 0;
}

static int get_circle_length(SEXP circle_length)
{
	int circle_len;

	if (!IS_INTEGER(circle_length) || LENGTH(circle_length) != 1)
		error("'circle_length' must be a single integer");
	circle_len = INTEGER(circle_length)[0];
	if (circle_len != NA_INTEGER && circle_len <= 0)
		error("'circle_length' must be a single "
                      "positive integer or NA");
	return circle_len;
}


/****************************************************************************
 * Move raw hits from buffers to Hits object or integer vector
 */

static SEXP new_direct_out(int q_len, int select_mode)
{
	SEXP ans;
	int init_val, i, *ans_elt;

	PROTECT(ans = NEW_INTEGER(q_len));
	init_val = select_mode == COUNT_HITS ? 0 : NA_INTEGER;
	for (i = 0, ans_elt = INTEGER(ans); i < q_len; i++, ans_elt++)
		*ans_elt = init_val;
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * NCList_find_overlaps()
 *
 * --- .Call ENTRY POINT ---
 * Args:
 *   q_start, q_end: Integer vectors of length M.
 *   s_start, s_end: Integer vectors of length N.
 *   nclist:         An integer vector representing the Nested Containment
 *                   List for 'y'.
 *   nclist_is_q:    TRUE or FALSE.
 *   min_score:      See get_min_overlap_score() C function.
 *   type:           See get_overlap_type() C function.
 *   select:         See _get_select_mode() C function in S4Vectors.
 *   circle_length:  A single positive integer or NA_INTEGER.
 */
SEXP NCList_find_overlaps(SEXP q_start, SEXP q_end,
			  SEXP s_start, SEXP s_end,
			  SEXP nclist, SEXP nclist_is_q,
			  SEXP min_score, SEXP type, SEXP select,
			  SEXP circle_length)
{
	int q_len, s_len,
	    min_overlap_score, overlap_type, select_mode, circle_len,
	    *direct_out, pp_is_q;
	const int *q_start_p, *q_end_p, *s_start_p, *s_end_p;
	IntAE qh_buf, sh_buf;
	SEXP ans;

	q_len = check_integer_pairs(q_start, q_end,
				    &q_start_p, &q_end_p,
				    "start(q)", "end(q)");
	s_len = check_integer_pairs(s_start, s_end,
				    &s_start_p, &s_end_p,
				    "start(s)", "end(s)");
	min_overlap_score = get_min_overlap_score(min_score);
	overlap_type = get_overlap_type(type);
	select_mode = get_select_mode(select);
	circle_len = get_circle_length(circle_length);

	qh_buf = new_IntAE(0, 0, 0);
	sh_buf = new_IntAE(0, 0, 0);
	direct_out = NULL;
	if (select_mode != ALL_HITS) {
		PROTECT(ans = new_direct_out(q_len, select_mode));
		direct_out = INTEGER(ans);
	}
	//init_clock("find_overlaps: T2 = ");
	pp_is_q = find_overlaps(
		q_start_p, q_end_p, NULL, NULL, q_len,
		s_start_p, s_end_p, NULL, NULL, s_len,
		min_overlap_score, overlap_type,
		select_mode, circle_len,
		nclist, LOGICAL(nclist_is_q)[0],
		&qh_buf, &sh_buf, direct_out);
	//print_elapsed_time();
	if (select_mode != ALL_HITS) {
		UNPROTECT(1);
		return ans;
	}
	return new_Hits(qh_buf.elts, sh_buf.elts, IntAE_get_nelt(&qh_buf),
			q_len, s_len, !pp_is_q);
}


/****************************************************************************
 * NCList_find_overlaps_in_groups()
 *
 * --- .Call ENTRY POINT ---
 * Args:
 *   q_start, q_end, q_space: Integer vectors of length M (or NULL for
 *                   'q_space').
 *   q_groups:       A CompressedIntegerList object of length NG1. Each list
 *                   element (integer vector) represents a group of 0-based
 *                   indices into 'q_start', 'q_end', and 'q_space'.
 *   s_start, s_end, s_space: Integer vectors of length N (or NULL for
 *                   's_space').
 *   s_groups:       A CompressedIntegerList object of length NG2. Each list
 *                   element (integer vector) represents a group of 0-based
 *                   indices into 's_start', 's_end', and 's_space'.
 *   nclists:        A list of length >= min(NG1, NG2). Each list element must
 *                   be NULL or an integer vector representing a Nested
 *                   Containment List.
 *   nclist_is_q:    A logical vector parallel to 'nclists'.
 *   min_score:      See get_min_overlap_score() C function.
 *   type:           See get_overlap_type() C function.
 *   select:         See _get_select_mode() C function in S4Vectors.
 *   circle_length:  An integer vector of length >= min(NG1, NG2) with positive
 *                   or NA values.
 */
SEXP NCList_find_overlaps_in_groups(
		SEXP q_start, SEXP q_end, SEXP q_space, SEXP q_groups,
		SEXP s_start, SEXP s_end, SEXP s_space, SEXP s_groups,
		SEXP nclists, SEXP nclist_is_q,
		SEXP min_score, SEXP type, SEXP select,
		SEXP circle_length)
{
	int q_len, s_len, NG1, NG2,
	    min_overlap_score, overlap_type, select_mode,
	    NG, i, qi_len, si_len, *direct_out;
	const int *q_start_p, *q_end_p, *q_space_p,
		  *s_start_p, *s_end_p, *s_space_p;
	CompressedIntsList_holder q_groups_holder, s_groups_holder;
	Ints_holder qi_group_holder, si_group_holder;
	IntAE qh_buf, sh_buf;
	SEXP ans;

	/* Check query. */
	q_len = check_integer_pairs(q_start, q_end,
				    &q_start_p, &q_end_p,
				    "start(query)", "end(query)");
	if (q_space == R_NilValue) {
		q_space_p = NULL;
	} else {
		if (LENGTH(q_space) != q_len)
			error("'query.space' must have the length "
			      "of 'query'");
		q_space_p = INTEGER(q_space);
	}
	q_groups_holder = _hold_CompressedIntegerList(q_groups);
	NG1 = _get_length_from_CompressedIntsList_holder(&q_groups_holder);

	/* Check subject. */
	s_len = check_integer_pairs(s_start, s_end,
				    &s_start_p, &s_end_p,
				    "start(s)", "end(s)");
	if (s_space == R_NilValue) {
		s_space_p = NULL;
	} else {
		if (LENGTH(s_space) != s_len)
			error("'subject.space' must have the length "
			      "of 'subject'");
		s_space_p = INTEGER(s_space);
	}
	s_groups_holder = _hold_CompressedIntegerList(s_groups);
	NG2 = _get_length_from_CompressedIntsList_holder(&s_groups_holder);

	min_overlap_score = get_min_overlap_score(min_score);
	overlap_type = get_overlap_type(type);
	select_mode = get_select_mode(select);

	qh_buf = new_IntAE(0, 0, 0);
	sh_buf = new_IntAE(0, 0, 0);
	direct_out = NULL;
	if (select_mode != ALL_HITS) {
		PROTECT(ans = new_direct_out(q_len, select_mode));
		direct_out = INTEGER(ans);
	}
	NG = NG1 <= NG2 ? NG1 : NG2;
	for (i = 0; i < NG; i++) {
		qi_group_holder = _get_elt_from_CompressedIntsList_holder(
					&q_groups_holder, i);
		qi_len = qi_group_holder.length;
		si_group_holder = _get_elt_from_CompressedIntsList_holder(
					&s_groups_holder, i);
		si_len = si_group_holder.length;
		find_overlaps(
			q_start_p, q_end_p, q_space_p,
			qi_group_holder.ptr, qi_len,
			s_start_p, s_end_p, s_space_p,
			si_group_holder.ptr, si_len,
			min_overlap_score, overlap_type,
			select_mode, INTEGER(circle_length)[i],
			VECTOR_ELT(nclists, i), LOGICAL(nclist_is_q)[i],
			&qh_buf, &sh_buf, direct_out);
	}
	if (select_mode != ALL_HITS) {
		UNPROTECT(1);
		return ans;
	}
	return new_Hits(qh_buf.elts, sh_buf.elts, IntAE_get_nelt(&qh_buf),
			q_len, s_len, 0);
}


/****************************************************************************
 Algorithm complexity
 ====================

   X: length of object to preprocess
   Y: length of other object
   H: nb of hits (upper bound is X * Y)

   Time of preprocessing:

     T1 = a * X * log(X)

   Time of find_overlaps(..., select="all"):

     T2 = b * Y * log(X) + c * H

   Total time T is T1 + T2.
 ****************************************************************************/


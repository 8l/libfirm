/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   Statistics for Firm. Distribution tables.
 * @author  Michael Beck
 */
#include <limits.h>

#include "hashptr.h"
#include "util.h"
#include "xmalloc.h"
#include "firmstat_t.h"

/**
 * calculates a hash value for an address
 */
static unsigned addr_hash(const void *object)
{
	return hash_ptr(object);
}

/**
 * calculates a hash value for an integer
 */
static unsigned int_hash(const void *object)
{
	return (unsigned)PTR_TO_INT(object);
}

/**
 * compare function for integer distribution tables
 */
static int int_cmp_fun(const void *elt, const void *key)
{
	const distrib_entry_t *p1 = (const distrib_entry_t*)elt;
	const distrib_entry_t *p2 = (const distrib_entry_t*)key;

	if (p1->object == p2->object)
		return 0;
	return p1->object < p2->object ? -1 : 1;
}

/*
 * create a new distribution table
 */
distrib_tbl_t *stat_new_distrib_tbl(pset_cmp_fun cmp_func, distrib_hash_fun hash_func)
{
	distrib_tbl_t *res = XMALLOC(distrib_tbl_t);

	obstack_init(&res->cnts);

	/* create the hash-table */
	res->hash_map  = new_pset(cmp_func, 8);
	res->hash_func = hash_func ? hash_func : addr_hash;
	res->int_dist  = 0;

	return res;
}

/*
 * create a new distribution table for an integer distribution
 */
distrib_tbl_t *stat_new_int_distrib_tbl(void)
{
	distrib_tbl_t *res = stat_new_distrib_tbl(int_cmp_fun, int_hash);

	if (res)
		res->int_dist = 1;

	return res;
}

/*
 * destroy a distribution table
 */
void stat_delete_distrib_tbl(distrib_tbl_t *tbl)
{
	if (tbl) {
		/* free all entries */
		obstack_free(&tbl->cnts, NULL);

		/* delete the hash table */
		del_pset(tbl->hash_map);
	}
}

/**
 * Returns the associates distrib_entry_t for an object
 */
static distrib_entry_t *distrib_get_entry(distrib_tbl_t *tbl, const void *object)
{
	distrib_entry_t key;
	distrib_entry_t *elem;

	key.object = object;

	elem = (distrib_entry_t*)pset_find(tbl->hash_map, &key, tbl->hash_func(object));
	if (elem)
		return elem;

	elem = OALLOC(&tbl->cnts, distrib_entry_t);

	/* clear counter */
	cnt_clr(&elem->cnt);

	elem->object = object;

	return (distrib_entry_t*)pset_insert(tbl->hash_map, elem, tbl->hash_func(object));
}

/*
 * increases object count by one
 */
void stat_inc_distrib_tbl(distrib_tbl_t *tbl, const void *object)
{
	distrib_entry_t *elem = distrib_get_entry(tbl, object);

	cnt_inc(&elem->cnt);
}

/*
 * increases key count by one
 */
void stat_inc_int_distrib_tbl(distrib_tbl_t *tbl, int key)
{
	stat_inc_distrib_tbl(tbl, INT_TO_PTR(key));
}

/**
 * iterates over all entries in a distribution table
 */
void stat_iterate_distrib_tbl(const distrib_tbl_t *tbl, eval_distrib_entry_fun eval, void *env)
{
	foreach_pset(tbl->hash_map, distrib_entry_t, entry)
		eval(entry, env);
}

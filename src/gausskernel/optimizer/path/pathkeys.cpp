/* -------------------------------------------------------------------------
 *
 * pathkeys.cpp
 *	  Utilities for matching and building path keys
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/path/pathkeys.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/skey.h"
#include "catalog/pg_opfamily.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/streamplan.h"
#include "optimizer/tlist.h"
#include "parser/parse_hint.h"
#include "pgxc/pgxc.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

static PathKey* makePathKey(EquivalenceClass* eclass, Oid opfamily, int strategy, bool nulls_first);
static bool pathkey_is_redundant(PathKey* new_pathkey, List* pathkeys, bool predpush = false);
static bool right_merge_direction(PlannerInfo* root, PathKey* pathkey);

/****************************************************************************
 *		PATHKEY CONSTRUCTION AND REDUNDANCY TESTING
 ****************************************************************************/
/*
 * makePathKey
 *		create a PathKey node
 *
 * This does not promise to create a canonical PathKey, it's merely a
 * convenience routine to build the specified node.
 */
static PathKey* makePathKey(EquivalenceClass* eclass, Oid opfamily, int strategy, bool nulls_first)
{
    PathKey* pk = makeNode(PathKey);

    pk->pk_eclass = eclass;
    pk->pk_opfamily = opfamily;
    pk->pk_strategy = strategy;
    pk->pk_nulls_first = nulls_first;

    return pk;
}

/*
 * make_canonical_pathkey
 *	  Given the parameters for a PathKey, find any pre-existing matching
 *	  pathkey in the query's list of "canonical" pathkeys.  Make a new
 *	  entry if there's not one already.
 *
 * Note that this function must not be used until after we have completed
 * merging EquivalenceClasses.
 */
PathKey* make_canonical_pathkey(
    PlannerInfo* root, EquivalenceClass* eclass, Oid opfamily, int strategy, bool nulls_first)
{
    PathKey* pk = NULL;
    ListCell* lc = NULL;
    MemoryContext oldcontext;

    /* The passed eclass might be non-canonical, so chase up to the top */
    while (eclass->ec_merged)
        eclass = eclass->ec_merged;

    foreach (lc, root->canon_pathkeys) {
        /* Here need ensure ec_group_set be also equal. */
        pk = (PathKey*)lfirst(lc);
        if (eclass == pk->pk_eclass && eclass->ec_group_set == pk->pk_eclass->ec_group_set &&
            OpFamilyEquals(opfamily, pk->pk_opfamily) && strategy == pk->pk_strategy &&
            nulls_first == pk->pk_nulls_first)
            return pk;
    }

    /*
     * Be sure canonical pathkeys are allocated in the main planning context.
     * Not an issue in normal planning, but it is for GEQO.
     */
    oldcontext = MemoryContextSwitchTo(root->planner_cxt);

    pk = makePathKey(eclass, opfamily, strategy, nulls_first);
    root->canon_pathkeys = lappend(root->canon_pathkeys, pk);

    (void)MemoryContextSwitchTo(oldcontext);

    return pk;
}

/*
 * pathkey_is_redundant
 *	   Is a pathkey redundant with one already in the given list?
 *
 * Both the given pathkey and the list members must be canonical for this
 * to work properly.  We detect two cases:
 *
 * 1. If the new pathkey's equivalence class contains a constant, and isn't
 * below an outer join, then we can disregard it as a sort key.  An example:
 *			SELECT ... WHERE x = 42 ORDER BY x, y;
 * We may as well just sort by y.  Note that because of opfamily matching,
 * this is semantically correct: we know that the equality constraint is one
 * that actually binds the variable to a single value in the terms of any
 * ordering operator that might go with the eclass.  This rule not only lets
 * us simplify (or even skip) explicit sorts, but also allows matching index
 * sort orders to a query when there are don't-care index columns.
 *
 * 2. If the new pathkey's equivalence class is the same as that of any
 * existing member of the pathkey list, then it is redundant.  Some examples:
 *			SELECT ... ORDER BY x, x;
 *			SELECT ... ORDER BY x, x DESC;
 *			SELECT ... WHERE x = y ORDER BY x, y;
 * In all these cases the second sort key cannot distinguish values that are
 * considered equal by the first, and so there's no point in using it.
 * Note in particular that we need not compare opfamily (all the opfamilies
 * of the EC have the same notion of equality) nor sort direction.
 *
 * Because the equivclass.c machinery forms only one copy of any EC per query,
 * pointer comparison is enough to decide whether canonical ECs are the same.
 */
static bool pathkey_is_redundant(PathKey* new_pathkey, List* pathkeys, bool predpush)
{
    EquivalenceClass* new_ec = new_pathkey->pk_eclass;
    ListCell* lc = NULL;

    /* Assert we've been given canonical pathkeys */
    Assert(!new_ec->ec_merged);

    /* Check for EC containing a constant --- unconditionally redundant */
    if (predpush) {
        /* skip the Param */
        bool have_const = false;
        if (EC_MUST_BE_REDUNDANT(new_ec))
        {
            lc = NULL;
            foreach (lc, new_ec->ec_members) {
                EquivalenceMember *mem = (EquivalenceMember *)lfirst(lc);
                if (mem->em_is_const && !check_param_clause((Node *)mem->em_expr)) {
                    have_const = true;
                    break;
                }
            }
        }

        if ((have_const && !new_ec->ec_below_outer_join) && !new_ec->ec_group_set)
            return true;

    } else {
        if (EC_MUST_BE_REDUNDANT(new_ec) && !new_ec->ec_group_set)
          return true;
    }

    /* If same EC already used in list, then redundant */
    foreach (lc, pathkeys) {
        PathKey* old_pathkey = (PathKey*)lfirst(lc);

        /* Assert we've been given canonical pathkeys */
        Assert(!old_pathkey->pk_eclass->ec_merged);

        if (new_ec == old_pathkey->pk_eclass)
            return true;
    }

    return false;
}

/*
 * canonicalize_pathkeys
 *	   Convert a not-necessarily-canonical pathkeys list to canonical form.
 *
 * Note that this function must not be used until after we have completed
 * merging EquivalenceClasses.
 *
 * aboveAgg marks whether this operation(sort or window funtion) is above agg.
 * For example: select a, sum(b) from t1 group by a order by 1, 2;
 * This order by operator is above agg.
 */
List* canonicalize_pathkeys(PlannerInfo* root, List* pathkeys)
{
    List* new_pathkeys = NIL;
    ListCell* l = NULL;

    foreach (l, pathkeys) {
        PathKey* pathkey = (PathKey*)lfirst(l);
        EquivalenceClass* eclass = NULL;
        PathKey* cpathkey = NULL;

        /* Find the canonical (merged) EquivalenceClass */
        eclass = pathkey->pk_eclass;
        while (eclass->ec_merged)
            eclass = eclass->ec_merged;

        /*
         * If we can tell it's redundant just from the EC, skip.
         * pathkey_is_redundant would notice that, but we needn't even bother
         * constructing the node...
         */
        if (EC_MUST_BE_REDUNDANT(eclass) && !eclass->ec_group_set)
            continue;

        /* OK, build a canonicalized PathKey struct */
        cpathkey =
            make_canonical_pathkey(root, eclass, pathkey->pk_opfamily, pathkey->pk_strategy, pathkey->pk_nulls_first);

        /* Add to list unless redundant */
        if (!pathkey_is_redundant(cpathkey, new_pathkeys))
            new_pathkeys = lappend(new_pathkeys, cpathkey);
    }
    return new_pathkeys;
}

/*
 * There maybe some CONST/PARAM EC in the pathkeys, it should be removed.
 */
List* remove_param_pathkeys(PlannerInfo* root, List* pathkeys)
{
    List* new_pathkeys = NIL;
    ListCell* l = NULL;

    if (pathkeys == NULL)
        return NULL;

    foreach (l, pathkeys) {
        PathKey* pathkey = (PathKey*)lfirst(l);
        EquivalenceClass* eclass = NULL;

        /* Find the canonical (merged) EquivalenceClass */
        eclass = pathkey->pk_eclass;
        Assert(eclass->ec_merged == NULL);

        /*
         * If we can tell it's redundant just from the EC, skip.
         * pathkey_is_redundant would notice that, but we needn't even bother
         * constructing the node...
         */
        if (EC_MUST_BE_REDUNDANT(eclass) && !eclass->ec_group_set)
            continue;

        new_pathkeys = lappend(new_pathkeys, pathkey);
    }
    return new_pathkeys;
}

/*
 * make_pathkey_from_sortinfo
 *	  Given an expression and sort-order information, create a PathKey.
 *	  If canonicalize = true, the result is a "canonical" PathKey,
 *	  otherwise not.  (But note it might be redundant anyway.)
 *
 * If the PathKey is being generated from a SortGroupClause, sortref should be
 * the SortGroupClause's SortGroupRef; otherwise zero.
 *
 * If rel is not NULL, it identifies a specific relation we're considering
 * a path for, and indicates that child EC members for that relation can be
 * considered.	Otherwise child members are ignored.  (See the comments for
 * get_eclass_for_sort_expr.)
 *
 * create_it is TRUE if we should create any missing EquivalenceClass
 * needed to represent the sort key.  If it's FALSE, we return NULL if the
 * sort key isn't already present in any EquivalenceClass.
 *
 * canonicalize should always be TRUE after EquivalenceClass merging has
 * been performed, but FALSE if we haven't done EquivalenceClass merging yet.
 */
static PathKey* make_pathkey_from_sortinfo(PlannerInfo* root, Expr* expr, Oid opfamily, Oid opcintype, Oid collation,
    bool reverse_sort, bool nulls_first, Index sortref, bool groupSet, Relids rel, bool create_it, bool canonicalize)
{
    int16 strategy;
    Oid equality_op;
    List* opfamilies = NIL;
    EquivalenceClass* eclass = NULL;

    strategy = reverse_sort ? BTGreaterStrategyNumber : BTLessStrategyNumber;

    /*
     * EquivalenceClasses need to contain opfamily lists based on the family
     * membership of mergejoinable equality operators, which could belong to
     * more than one opfamily.	So we have to look up the opfamily's equality
     * operator and get its membership.
     */
    equality_op = get_opfamily_member(opfamily, opcintype, opcintype, BTEqualStrategyNumber);
    if (!OidIsValid(equality_op)) /* shouldn't happen */
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                (errmsg(
                    "could not find equality operator for opfamily %u when make pathkey from sortinfo", opfamily))));
    opfamilies = get_mergejoin_opfamilies(equality_op);
    if (opfamilies == NIL) /* certainly should find some */
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                (errmsg("could not find opfamilies for equality operator %u when make pathkey from sortinfo",
                    equality_op))));
    /* Now find or (optionally) create a matching EquivalenceClass */
    eclass = get_eclass_for_sort_expr(root, expr, opfamilies, opcintype, collation, sortref, groupSet, rel, create_it);

    /* Fail if no EC and !create_it */
    if (eclass == NULL)
        return NULL;

    /* And finally we can find or create a PathKey node */
    if (canonicalize)
        return make_canonical_pathkey(root, eclass, opfamily, strategy, nulls_first);
    else
        return makePathKey(eclass, opfamily, strategy, nulls_first);
}

/*
 * make_pathkey_from_sortop
 *	  Like make_pathkey_from_sortinfo, but work from a sort operator.
 *
 * This should eventually go away, but we need to restructure SortGroupClause
 * first.
 */
static PathKey* make_pathkey_from_sortop(PlannerInfo* root, Expr* expr, Oid ordering_op, bool nulls_first,
    Index sortref, bool groupSet, bool create_it, bool canonicalize)
{
    Oid opfamily, opcintype, collation;
    int16 strategy;

    /* Find the operator in pg_amop --- failure shouldn't happen */
    if (!get_ordering_op_properties(ordering_op, &opfamily, &opcintype, &strategy))
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                (errmsg("operator %u is not a valid ordering operator when make pathkey from sortinfo", ordering_op))));

    /* Because SortGroupClause doesn't carry collation, consult the expr */
    collation = exprCollation((Node*)expr);

    return make_pathkey_from_sortinfo(root,
        expr,
        opfamily,
        opcintype,
        collation,
        (strategy == BTGreaterStrategyNumber),
        nulls_first,
        sortref,
        groupSet,
        NULL,
        create_it,
        canonicalize);
}

/****************************************************************************
 *		PATHKEY COMPARISONS
 ****************************************************************************/
/*
 * compare_pathkeys
 *	  Compare two pathkeys to see if they are equivalent, and if not whether
 *	  one is "better" than the other.
 *
 *	  This function may only be applied to canonicalized pathkey lists.
 *	  In the canonical representation, pathkeys can be checked for equality
 *	  by simple pointer comparison.
 */
PathKeysComparison compare_pathkeys(List* keys1, List* keys2)
{
    ListCell* key1 = NULL;
    ListCell* key2 = NULL;

    /*
     * Fall out quickly if we are passed two identical lists.  This mostly
     * catches the case where both are NIL, but that's common enough to
     * warrant the test.
     */
    if (keys1 == keys2)
        return PATHKEYS_EQUAL;

    forboth(key1, keys1, key2, keys2)
    {
        PathKey* pathkey1 = (PathKey*)lfirst(key1);
        PathKey* pathkey2 = (PathKey*)lfirst(key2);

        /*
         * XXX would like to check that we've been given canonicalized input,
         * but PlannerInfo not accessible here...
         */
#ifdef NOT_USED
        AssertEreport(list_member_ptr(root->canon_pathkeys, pathkey1), MOD_OPT, "pathky1 is not a member in pathkeys");

        AssertEreport(list_member_ptr(root->canon_pathkeys, pathkey2), MOD_OPT, "pathky2 is not a member in pathkeys");
#endif
        if (pathkey1 == pathkey2) {
            continue;
        }
        if (pathkey1 == NULL && pathkey2 != NULL) {
            return PATHKEYS_DIFFERENT; /* no need to keep looking */
        }
        if (pathkey1 != NULL && pathkey2 == NULL) {
            return PATHKEYS_DIFFERENT; /* no need to keep looking */
        }
        if (pathkey1->type != pathkey2->type || !OpFamilyEquals(pathkey1->pk_opfamily, pathkey2->pk_opfamily) ||
            pathkey1->pk_eclass != pathkey2->pk_eclass || pathkey1->pk_strategy != pathkey2->pk_strategy ||
            pathkey1->pk_nulls_first != pathkey2->pk_nulls_first) {
            return PATHKEYS_DIFFERENT; /* no need to keep looking */
        }
    }

    /*
     * If we reached the end of only one list, the other is longer and
     * therefore not a subset.
     */
    if (key1 != NULL)
        return PATHKEYS_BETTER1; /* key1 is longer */
    if (key2 != NULL)
        return PATHKEYS_BETTER2; /* key2 is longer */
    return PATHKEYS_EQUAL;
}

/*
 * pathkeys_contained_in
 *	  Common special case of compare_pathkeys: we just want to know
 *	  if keys2 are at least as well sorted as keys1.
 */
bool pathkeys_contained_in(List* keys1, List* keys2)
{
    switch (compare_pathkeys(keys1, keys2)) {
        case PATHKEYS_EQUAL:
        case PATHKEYS_BETTER2:
            return true;
        default:
            break;
    }
    return false;
}

/*
 * get_cheapest_path_for_pathkeys
 *	  Find the cheapest path (according to the specified criterion) that
 *	  satisfies the given pathkeys and parameterization.
 *	  Return NULL if no such path.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'required_outer' denotes allowable outer relations for parameterized paths
 * 'cost_criterion' is STARTUP_COST or TOTAL_COST
 */
Path* get_cheapest_path_for_pathkeys(List* paths, List* pathkeys, Relids required_outer, CostSelector cost_criterion)
{
    Path* matched_path = NULL;
    ListCell* l = NULL;

    foreach (l, paths) {
        Path* path = (Path*)lfirst(l);

        /*
         * Since cost comparison is a lot cheaper than pathkey comparison, do
         * that first.	(XXX is that still true?)
         */
        if (matched_path != NULL && compare_path_costs(matched_path, path, cost_criterion) <= 0)
            continue;

        if (pathkeys_contained_in(pathkeys, path->pathkeys) && bms_is_subset(PATH_REQ_OUTER(path), required_outer))
            matched_path = path;
    }
    return matched_path;
}

/*
 * get_cheapest_fractional_path_for_pathkeys
 *	  Find the cheapest path (for retrieving a specified fraction of all
 *	  the tuples) that satisfies the given pathkeys and parameterization.
 *	  Return NULL if no such path.
 *
 * See compare_fractional_path_costs() for the interpretation of the fraction
 * parameter.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'required_outer' denotes allowable outer relations for parameterized paths
 * 'fraction' is the fraction of the total tuples expected to be retrieved
 */
Path* get_cheapest_fractional_path_for_pathkeys(List* paths, List* pathkeys, Relids required_outer, double fraction)
{
    Path* matched_path = NULL;
    ListCell* l = NULL;

    foreach (l, paths) {
        Path* path = (Path*)lfirst(l);

        /*
         * Since cost comparison is a lot cheaper than pathkey comparison, do
         * that first.	(XXX is that still true?)
         */
        if (matched_path != NULL && compare_fractional_path_costs(matched_path, path, fraction) <= 0)
            continue;

        if (pathkeys_contained_in(pathkeys, path->pathkeys) && bms_is_subset(PATH_REQ_OUTER(path), required_outer))
            matched_path = path;
    }
    return matched_path;
}

/****************************************************************************
 *		NEW PATHKEY FORMATION
 ****************************************************************************/
/*
 * build_index_pathkeys
 *	  Build a pathkeys list that describes the ordering induced by an index
 *	  scan using the given index.  (Note that an unordered index doesn't
 *	  induce any ordering, so we return NIL.)
 *
 * If 'scandir' is BackwardScanDirection, build pathkeys representing a
 * backwards scan of the index.
 *
 * We iterate only key columns of covering indexes, since non-key columns
 * don't influence index ordering.  The result is canonical, meaning that
 * redundant pathkeys are removed; it may therefore have fewer entries than
 * there are key columns in the index.
 *
 * Another reason for stopping early is that we may be able to tell that
 * an index column's sort order is uninteresting for this query.  However,
 * that test is just based on the existence of an EquivalenceClass and not
 * on position in pathkey lists, so it's not complete.  Caller should call
 * truncate_useless_pathkeys() to possibly remove more pathkeys.
 */
List* build_index_pathkeys(PlannerInfo* root, IndexOptInfo* index, ScanDirection scandir)
{
    List* retval = NIL;
    ListCell* lc = NULL;
    int i;

    if (index->sortopfamily == NULL)
        return NIL; /* non-orderable index */

    i = 0;
    foreach (lc, index->indextlist) {
        TargetEntry* indextle = (TargetEntry*)lfirst(lc);
        Expr* indexkey = NULL;
        bool reverse_sort = false;
        bool nulls_first = false;
        PathKey* cpathkey = NULL;

        /*
         * INCLUDE columns are stored in index unordered, so they don't
         * support ordered index scan.
         */
        if (i >= index->nkeycolumns) {
            break;
        }

        /* We assume we don't need to make a copy of the tlist item */
        indexkey = indextle->expr;

        if (ScanDirectionIsBackward(scandir)) {
            reverse_sort = !index->reverse_sort[i];
            nulls_first = !index->nulls_first[i];
        } else {
            reverse_sort = index->reverse_sort[i];
            nulls_first = index->nulls_first[i];
        }
        
        /* 
         * in B format, null value in insert into the minimal partition
         * desc default: nulls first -> nulls last
         * asc  default: nulls last  -> nulls
         */
        if (index->ispartitionedindex && !index->isGlobal && CheckPluginNullsPolicy()) {
            if ((!reverse_sort && !nulls_first) || (reverse_sort && nulls_first)) {
                nulls_first = !nulls_first;
            }
        }

        /* OK, try to make a canonical pathkey for this sort key */
        cpathkey = make_pathkey_from_sortinfo(root,
            indexkey,
            index->sortopfamily[i],
            index->opcintype[i],
            index->indexcollations[i],
            reverse_sort,
            nulls_first,
            0,
            false,
            index->rel->relids,
            false,
            true);

        /*
         * If the sort key isn't already present in any EquivalenceClass, then
         * it's not an interesting sort order for this query.  So we can stop
         * now --- lower-order sort keys aren't useful either.
         */
        if (cpathkey == NULL)
            break;

        /* Add to list unless redundant */
        if (!pathkey_is_redundant(cpathkey, retval))
            retval = lappend(retval, cpathkey);

        i++;
    }

    return retval;
}

/*
 * convert_subquery_pathkeys
 *	  Build a pathkeys list that describes the ordering of a subquery's
 *	  result, in the terms of the outer query.	This is essentially a
 *	  task of conversion.
 *
 * 'rel': outer query's RelOptInfo for the subquery relation.
 * 'subquery_pathkeys': the subquery's output pathkeys, in its terms.
 *
 * It is not necessary for caller to do truncate_useless_pathkeys(),
 * because we select keys in a way that takes usefulness of the keys into
 * account.
 */
List* convert_subquery_pathkeys(PlannerInfo* root, RelOptInfo* rel, List* subquery_pathkeys)
{
    List* retval = NIL;
    int retvallen = 0;
    int outer_query_keys = list_length(root->query_pathkeys);
    List* sub_tlist = rel->subplan->targetlist;
    ListCell* i = NULL;

    foreach (i, subquery_pathkeys) {
        PathKey* sub_pathkey = (PathKey*)lfirst(i);
        EquivalenceClass* sub_eclass = sub_pathkey->pk_eclass;
        PathKey* best_pathkey = NULL;

        if (sub_eclass->ec_has_volatile) {
            /*
             * If the sub_pathkey's EquivalenceClass is volatile, then it must
             * have come from an ORDER BY clause, and we have to match it to
             * that same targetlist entry.
             */
            TargetEntry* tle = NULL;

            if (sub_eclass->ec_sortref == 0) /* can't happen */
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                        (errmsg("volatile EquivalenceClass has no sortref when convert subquery pathkeys"))));
            tle = get_sortgroupref_tle(sub_eclass->ec_sortref, sub_tlist);
            AssertEreport(tle != NULL, MOD_OPT, "tle is NULL");
            /* resjunk items aren't visible to outer query */
            if (!tle->resjunk) {
                /* We can represent this sub_pathkey */
                EquivalenceMember* sub_member = NULL;
                Expr* outer_expr = NULL;
                EquivalenceClass* outer_ec = NULL;

                AssertEreport(list_length(sub_eclass->ec_members) == 1, MOD_OPT, "ec member number is not 1");
                sub_member = (EquivalenceMember*)linitial(sub_eclass->ec_members);
                outer_expr = (Expr*)makeVarFromTargetEntry(rel->relid, tle);

                /*
                 * Note: it might look funny to be setting sortref = 0 for a
                 * reference to a volatile sub_eclass.	However, the
                 * expression is *not* volatile in the outer query: it's just
                 * a Var referencing whatever the subquery emitted. (IOW, the
                 * outer query isn't going to re-execute the volatile
                 * expression itself.)	So this is okay.
                 */
                outer_ec = get_eclass_for_sort_expr(root,
                    outer_expr,
                    sub_eclass->ec_opfamilies,
                    sub_member->em_datatype,
                    sub_eclass->ec_collation,
                    0,
                    false,
                    rel->relids,
                    false);

                /*
                 * If we don't find a matching EC, sub-pathkey isn't
                 * interesting to the outer query
                 */
                if (outer_ec != NULL)
                    best_pathkey = make_canonical_pathkey(root,
                        outer_ec,
                        sub_pathkey->pk_opfamily,
                        sub_pathkey->pk_strategy,
                        sub_pathkey->pk_nulls_first);
            }
        } else {
            /*
             * Otherwise, the sub_pathkey's EquivalenceClass could contain
             * multiple elements (representing knowledge that multiple items
             * are effectively equal).	Each element might match none, one, or
             * more of the output columns that are visible to the outer query.
             * This means we may have multiple possible representations of the
             * sub_pathkey in the context of the outer query.  Ideally we
             * would generate them all and put them all into an EC of the
             * outer query, thereby propagating equality knowledge up to the
             * outer query.  Right now we cannot do so, because the outer
             * query's EquivalenceClasses are already frozen when this is
             * called. Instead we prefer the one that has the highest "score"
             * (number of EC peers, plus one if it matches the outer
             * query_pathkeys). This is the most likely to be useful in the
             * outer query.
             */
            int best_score = -1;
            ListCell* j = NULL;

            foreach (j, sub_eclass->ec_members) {
                EquivalenceMember* sub_member = (EquivalenceMember*)lfirst(j);
                Expr* sub_expr = sub_member->em_expr;
                Oid sub_expr_type = sub_member->em_datatype;
                Oid sub_expr_coll = sub_eclass->ec_collation;
                ListCell* k = NULL;
                int seq = 0;

                if (sub_member->em_is_child)
                    continue; /* ignore children here */

                foreach (k, sub_tlist) {
                    TargetEntry* tle = (TargetEntry*)lfirst(k);
                    Expr* tle_expr = NULL;
                    Expr* outer_expr = NULL;
                    EquivalenceClass* outer_ec = NULL;
                    PathKey* outer_pk = NULL;
                    int score;
                    ListCell* lc = NULL;

                    seq++;

                    /* resjunk items aren't visible to outer query */
                    if (tle->resjunk)
                        continue;

                    /* check if targetentry exists in final subquery targetlist */
                    foreach (lc, rel->reltarget->exprs) {
                        Node* n = (Node*)lfirst(lc);
                        if (IsA(n, Var) && ((Var*)n)->varattno == seq)
                            break;
                    }

                    if (lc == NULL)
                        continue;

                    /*
                     * The targetlist entry is considered to match if it
                     * matches after sort-key canonicalization.  That is
                     * needed since the sub_expr has been through the same
                     * process.
                     */
                    tle_expr = canonicalize_ec_expression(tle->expr, sub_expr_type, sub_expr_coll);
                    if (!equal(tle_expr, sub_expr))
                        continue;

                    /*
                     * Build a representation of this targetlist entry as an
                     * outer Var.
                     */
                    outer_expr = (Expr*)makeVarFromTargetEntry(rel->relid, tle);

                    /* See if we have a matching EC for that */
                    outer_ec = get_eclass_for_sort_expr(root,
                        outer_expr,
                        sub_eclass->ec_opfamilies,
                        sub_expr_type,
                        sub_expr_coll,
                        0,
                        false,
                        rel->relids,
                        false);

                    /*
                     * If we don't find a matching EC, this sub-pathkey isn't
                     * interesting to the outer query
                     */
                    if (outer_ec == NULL)
                        continue;

                    outer_pk = make_canonical_pathkey(root,
                        outer_ec,
                        sub_pathkey->pk_opfamily,
                        sub_pathkey->pk_strategy,
                        sub_pathkey->pk_nulls_first);
                    /* score = # of equivalence peers */
                    score = list_length(outer_ec->ec_members) - 1;
                    /* +1 if it matches the proper query_pathkeys item */
                    if (retvallen < outer_query_keys && list_nth(root->query_pathkeys, retvallen) == outer_pk)
                        score++;
                    if (score > best_score) {
                        best_pathkey = outer_pk;
                        best_score = score;
                    }
                }
            }
        }

        /*
         * If we couldn't find a representation of this sub_pathkey, we're
         * done (we can't use the ones to its right, either).
         */
        if (best_pathkey == NULL)
            break;

        /*
         * Eliminate redundant ordering info; could happen if outer query
         * equivalences subquery keys...
         */
        if (!pathkey_is_redundant(best_pathkey, retval)) {
            retval = lappend(retval, best_pathkey);
            retvallen++;
        }
    }

    return retval;
}

/*
 * build_join_pathkeys
 *	  Build the path keys for a join relation constructed by mergejoin or
 *	  nestloop join.  This is normally the same as the outer path's keys.
 *
 *	  EXCEPTION: in a FULL or RIGHT join, we cannot treat the result as
 *	  having the outer path's path keys, because null lefthand rows may be
 *	  inserted at random points.  It must be treated as unsorted.
 *
 *	  We truncate away any pathkeys that are uninteresting for higher joins.
 *
 * 'joinrel' is the join relation that paths are being formed for
 * 'jointype' is the join type (inner, left, full, etc)
 * 'outer_pathkeys' is the list of the current outer path's path keys
 *
 * Returns the list of new path keys.
 */
List* build_join_pathkeys(PlannerInfo* root, RelOptInfo* joinrel, JoinType jointype, List* outer_pathkeys)
{
    if (jointype == JOIN_FULL || jointype == JOIN_RIGHT || jointype == JOIN_RIGHT_ANTI_FULL)
        return NIL;

    /*
     * This used to be quite a complex bit of code, but now that all pathkey
     * sublists start out life canonicalized, we don't have to do a darn thing
     * here!
     *
     * We do, however, need to truncate the pathkeys list, since it may
     * contain pathkeys that were useful for forming this joinrel but are
     * uninteresting to higher levels.
     */
    return truncate_useless_pathkeys(root, joinrel, outer_pathkeys);
}

/****************************************************************************
 *		PATHKEYS AND SORT CLAUSES
 ****************************************************************************/
/*
 * make_pathkeys_for_sortclauses
 *		Generate a pathkeys list that represents the sort order specified
 *		by a list of SortGroupClauses
 *
 * If canonicalize is TRUE, the resulting PathKeys are all in canonical form;
 * otherwise not.  canonicalize should always be TRUE after EquivalenceClass
 * merging has been performed, but FALSE if we haven't done EquivalenceClass
 * merging yet.  (We provide this option because grouping_planner() needs to
 * be able to represent requested pathkeys before the equivalence classes have
 * been created for the query.)
 *
 * 'sortclauses' is a list of SortGroupClause nodes
 * 'tlist' is the targetlist to find the referenced tlist entries in
 */
List* make_pathkeys_for_sortclauses(PlannerInfo* root, List* sortclauses, List* tlist, bool canonicalize)
{
    List* pathkeys = NIL;
    ListCell* l = NULL;

    foreach (l, sortclauses) {
        SortGroupClause* sortcl = (SortGroupClause*)lfirst(l);
        Expr* sortkey = NULL;
        PathKey* pathkey = NULL;

        sortkey = (Expr*)get_sortgroupclause_expr(sortcl, tlist);
        AssertEreport(OidIsValid(sortcl->sortop), MOD_OPT, "ordering operator is invalid");
        pathkey = make_pathkey_from_sortop(root,
            sortkey,
            sortcl->sortop,
            sortcl->nulls_first,
            sortcl->tleSortGroupRef,
            sortcl->groupSet,
            true,
            canonicalize);

        /* Canonical form eliminates redundant ordering keys */
        if (canonicalize) {
            if (!pathkey_is_redundant(pathkey, pathkeys, ENABLE_PRED_PUSH_ALL(root)))
                pathkeys = lappend(pathkeys, pathkey);
        } else
            pathkeys = lappend(pathkeys, pathkey);
    }
    return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES
 ****************************************************************************/
/*
 * initialize_mergeclause_eclasses
 *		Set the EquivalenceClass links in a mergeclause restrictinfo.
 *
 * RestrictInfo contains fields in which we may cache pointers to
 * EquivalenceClasses for the left and right inputs of the mergeclause.
 * (If the mergeclause is a true equivalence clause these will be the
 * same EquivalenceClass, otherwise not.)  If the mergeclause is either
 * used to generate an EquivalenceClass, or derived from an EquivalenceClass,
 * then it's easy to set up the left_ec and right_ec members --- otherwise,
 * this function should be called to set them up.  We will generate new
 * EquivalenceClauses if necessary to represent the mergeclause's left and
 * right sides.
 *
 * Note this is called before EC merging is complete, so the links won't
 * necessarily point to canonical ECs.	Before they are actually used for
 * anything, update_mergeclause_eclasses must be called to ensure that
 * they've been updated to point to canonical ECs.
 */
void initialize_mergeclause_eclasses(PlannerInfo* root, RestrictInfo* restrictinfo)
{
    Expr* clause = restrictinfo->clause;
    Oid lefttype, righttype;

    /* Should be a mergeclause ... */
    AssertEreport(restrictinfo->mergeopfamilies != NIL, MOD_OPT, "clause is not mergejoinable");
    /* ... with links not yet set */
    AssertEreport(restrictinfo->left_ec == NULL, MOD_OPT, "lefthand mergeclause processing is set");
    AssertEreport(restrictinfo->right_ec == NULL, MOD_OPT, "righthand mergeclause processing is set");

    /* Need the declared input types of the operator */
    op_input_types(((OpExpr*)clause)->opno, &lefttype, &righttype);

    /* Find or create a matching EquivalenceClass for each side */
    restrictinfo->left_ec = get_eclass_for_sort_expr(root,
        (Expr*)get_leftop(clause),
        restrictinfo->mergeopfamilies,
        lefttype,
        ((OpExpr*)clause)->inputcollid,
        0,
        false,
        NULL,
        true);
    restrictinfo->right_ec = get_eclass_for_sort_expr(root,
        (Expr*)get_rightop(clause),
        restrictinfo->mergeopfamilies,
        righttype,
        ((OpExpr*)clause)->inputcollid,
        0,
        false,
        NULL,
        true);
}

/*
 * update_mergeclause_eclasses
 *		Make the cached EquivalenceClass links valid in a mergeclause
 *		restrictinfo.
 *
 * These pointers should have been set by process_equivalence or
 * initialize_mergeclause_eclasses, but they might have been set to
 * non-canonical ECs that got merged later.  Chase up to the canonical
 * merged parent if so.
 */
void update_mergeclause_eclasses(PlannerInfo* root, RestrictInfo* restrictinfo)
{
    /* Should be a merge clause ... */
    AssertEreport(restrictinfo->mergeopfamilies != NIL, MOD_OPT, "clause is not mergejoinable");
    /* ... with pointers already set */
    AssertEreport(restrictinfo->left_ec != NULL, MOD_OPT, "lefthand mergeclause processing is not set");
    AssertEreport(restrictinfo->right_ec != NULL, MOD_OPT, "righthand mergeclause processing is not set");

    /* Chase up to the top as needed */
    while (restrictinfo->left_ec->ec_merged)
        restrictinfo->left_ec = restrictinfo->left_ec->ec_merged;
    while (restrictinfo->right_ec->ec_merged)
        restrictinfo->right_ec = restrictinfo->right_ec->ec_merged;
}

/*
 * find_mergeclauses_for_outer_pathkeys
 *	  This routine attempts to find a list of mergeclauses that can be
 *	  used with a specified ordering for the join's outer relation.
 *	  If successful, it returns a list of mergeclauses.
 *
 * 'pathkeys' is a pathkeys list showing the ordering of an outer-rel path.
 * 'restrictinfos' is a list of mergejoinable restriction clauses for the
 *			join relation being formed, in no particular order.
 *
 * The restrictinfos must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 *
 * The result is NIL if no merge can be done, else a maximal list of
 * usable mergeclauses (represented as a list of their restrictinfo nodes).
 * The list is ordered to match the pathkeys, as required for execution.
 */
List* find_mergeclauses_for_outer_pathkeys(PlannerInfo* root, List* pathkeys, List* restrictinfos)
{
    List* mergeclauses = NIL;
    ListCell* i = NULL;

    /* make sure we have eclasses cached in the clauses */
    foreach (i, restrictinfos) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(i);

        update_mergeclause_eclasses(root, rinfo);
    }

    foreach (i, pathkeys) {
        PathKey* pathkey = (PathKey*)lfirst(i);
        EquivalenceClass* pathkey_ec = pathkey->pk_eclass;
        List* matched_restrictinfos = NIL;
        ListCell* j = NULL;

        /* ----------
         * A mergejoin clause matches a pathkey if it has the same EC.
         * If there are multiple matching clauses, take them all.  In plain
         * inner-join scenarios we expect only one match, because
         * equivalence-class processing will have removed any redundant
         * mergeclauses.  However, in outer-join scenarios there might be
         * multiple matches.  An example is
         *
         *	select * from a full join b
         *		on a.v1 = b.v1 and a.v2 = b.v2 and a.v1 = b.v2;
         *
         * Given the pathkeys ({a.v1}, {a.v2}) it is okay to return all three
         * clauses (in the order a.v1=b.v1, a.v1=b.v2, a.v2=b.v2) and indeed
         * we *must* do so or we will be unable to form a valid plan.
         *
         * We expect that the given pathkeys list is canonical, which means
         * no two members have the same EC, so it's not possible for this
         * code to enter the same mergeclause into the result list twice.
         *
         * It's possible that multiple matching clauses might have different
         * ECs on the other side, in which case the order we put them into our
         * result makes a difference in the pathkeys required for the inner
         * input rel.  However this routine hasn't got any info about which
         * order would be best, so we don't worry about that.
         *
         * It's also possible that the selected mergejoin clauses produce
         * a noncanonical ordering of pathkeys for the inner side, ie, we
         * might select clauses that reference b.v1, b.v2, b.v1 in that
         * order.  This is not harmful in itself, though it suggests that
         * the clauses are partially redundant.  Since the alternative is
         * to omit mergejoin clauses and thereby possibly fail to generate a
         * plan altogether, we live with it.  make_inner_pathkeys_for_merge()
         * has to delete duplicates when it constructs the inner pathkeys
         * list, and we also have to deal with such cases specially
         * in create_mergejoin_plan().
         * ----------
         */
        foreach (j, restrictinfos) {
            RestrictInfo* rinfo = (RestrictInfo*)lfirst(j);
            EquivalenceClass* clause_ec = NULL;

            clause_ec = rinfo->outer_is_left ? rinfo->left_ec : rinfo->right_ec;
            if (clause_ec == pathkey_ec)
                matched_restrictinfos = lappend(matched_restrictinfos, rinfo);
        }

        /*
         * If we didn't find a mergeclause, we're done --- any additional
         * sort-key positions in the pathkeys are useless.	(But we can still
         * mergejoin if we found at least one mergeclause.)
         */
        if (matched_restrictinfos == NIL)
            break;

        /*
         * If we did find usable mergeclause(s) for this sort-key position,
         * add them to result list.
         */
        mergeclauses = list_concat(mergeclauses, matched_restrictinfos);
    }

    return mergeclauses;
}

inline int get_pathkey_index(EquivalenceClass** ecs, int necs, EquivalenceClass* key)
{
    int idx;
    for (idx = 0; idx < necs; idx++) {
        if (ecs[idx] == key)
            break; /* found match */
    }
    return idx;
}

/*
 * select_outer_pathkeys_for_merge
 *	  Builds a pathkey list representing a possible sort ordering
 *	  that can be used with the given mergeclauses.
 *
 * 'mergeclauses' is a list of RestrictInfos for mergejoin clauses
 *			that will be used in a merge join.
 * 'joinrel' is the join relation we are trying to construct.
 *
 * The restrictinfos must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 *
 * Returns a pathkeys list that can be applied to the outer relation.
 *
 * Since we assume here that a sort is required, there is no particular use
 * in matching any available ordering of the outerrel.	(joinpath.c has an
 * entirely separate code path for considering sort-free mergejoins.)  Rather,
 * it's interesting to try to match the requested query_pathkeys so that a
 * second output sort may be avoided; and failing that, we try to list "more
 * popular" keys (those with the most unmatched EquivalenceClass peers)
 * earlier, in hopes of making the resulting ordering useful for as many
 * higher-level mergejoins as possible.
 */
List* select_outer_pathkeys_for_merge(PlannerInfo* root, List* mergeclauses, RelOptInfo* joinrel)
{
    List* pathkeys = NIL;
    int nClauses = list_length(mergeclauses);
    EquivalenceClass** ecs;
    int* scores = NULL;
    int necs;
    ListCell* lc = NULL;
    int j;

    /* Might have no mergeclauses */
    if (nClauses == 0)
        return NIL;

    /*
     * Make arrays of the ECs used by the mergeclauses (dropping any
     * duplicates) and their "popularity" scores.
     */
    ecs = (EquivalenceClass**)palloc(nClauses * sizeof(EquivalenceClass*));
    scores = (int*)palloc(nClauses * sizeof(int));
    necs = 0;

    foreach (lc, mergeclauses) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(lc);
        EquivalenceClass* oeclass = NULL;
        int score;
        ListCell* lc2 = NULL;

        /* get the outer eclass */
        update_mergeclause_eclasses(root, rinfo);

        oeclass = (rinfo->outer_is_left) ? rinfo->left_ec : rinfo->right_ec;

        /* reject duplicates */
        j = get_pathkey_index(ecs, necs, oeclass);
        if (j < necs)
            continue;

        /* compute score */
        score = 0;
        foreach (lc2, oeclass->ec_members) {
            EquivalenceMember* em = (EquivalenceMember*)lfirst(lc2);

            /* Potential future join partner? */
            if (!em->em_is_const && !em->em_is_child && !bms_overlap(em->em_relids, joinrel->relids))
                score++;
        }

        ecs[necs] = oeclass;
        scores[necs] = score;
        necs++;
    }

    /*
     * Find out if we have all the ECs mentioned in query_pathkeys; if so we
     * can generate a sort order that's also useful for final output. There is
     * no percentage in a partial match, though, so we have to have 'em all.
     */
    if (root->query_pathkeys) {
        foreach (lc, root->query_pathkeys) {
            PathKey* query_pathkey = (PathKey*)lfirst(lc);
            EquivalenceClass* query_ec = query_pathkey->pk_eclass;

            j = get_pathkey_index(ecs, necs, query_ec);
            if (j >= necs)
                break; /* didn't find match */
        }
        /* if we got to the end of the list, we have them all */
        if (lc == NULL) {
            /* copy query_pathkeys as starting point for our output */
            pathkeys = list_copy(root->query_pathkeys);
            /* mark their ECs as already-emitted */
            foreach (lc, root->query_pathkeys) {
                PathKey* query_pathkey = (PathKey*)lfirst(lc);
                EquivalenceClass* query_ec = query_pathkey->pk_eclass;

                j = get_pathkey_index(ecs, necs, query_ec);
                if (j < necs) {
                    scores[j] = -1;
                }
            }
        }
    }

    /*
     * Add remaining ECs to the list in popularity order, using a default sort
     * ordering.  (We could use qsort() here, but the list length is usually
     * so small it's not worth it.)
     */
    for (;;) {
        int best_j;
        int best_score;
        EquivalenceClass* ec = NULL;
        PathKey* pathkey = NULL;

        best_j = 0;
        best_score = scores[0];
        for (j = 1; j < necs; j++) {
            if (scores[j] > best_score) {
                best_j = j;
                best_score = scores[j];
            }
        }
        if (best_score < 0)
            break; /* all done */
        ec = ecs[best_j];
        scores[best_j] = -1;
        pathkey = make_canonical_pathkey(root, ec, linitial_oid(ec->ec_opfamilies), BTLessStrategyNumber, false);
        /* can't be redundant because no duplicate ECs */
        AssertEreport(!pathkey_is_redundant(pathkey, pathkeys), MOD_OPT, "pathkey is redundant");
        pathkeys = lappend(pathkeys, pathkey);
    }

    pfree_ext(ecs);
    pfree_ext(scores);

    return pathkeys;
}

/*
 * make_inner_pathkeys_for_merge
 *	  Builds a pathkey list representing the explicit sort order that
 *	  must be applied to an inner path to make it usable with the
 *	  given mergeclauses.
 *
 * 'mergeclauses' is a list of RestrictInfos for the mergejoin clauses
 *			that will be used in a merge join, in order.
 * 'outer_pathkeys' are the already-known canonical pathkeys for the outer
 *			side of the join.
 *
 * The restrictinfos must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 *
 * Returns a pathkeys list that can be applied to the inner relation.
 *
 * Note that it is not this routine's job to decide whether sorting is
 * actually needed for a particular input path.  Assume a sort is necessary;
 * just make the keys, eh?
 */
List* make_inner_pathkeys_for_merge(PlannerInfo* root, List* mergeclauses, List* outer_pathkeys)
{
    List* pathkeys = NIL;
    EquivalenceClass* lastoeclass = NULL;
    PathKey* opathkey = NULL;
    ListCell* lc = NULL;
    ListCell* lop = NULL;

    lastoeclass = NULL;
    opathkey = NULL;
    lop = list_head(outer_pathkeys);

    foreach (lc, mergeclauses) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(lc);
        EquivalenceClass* oeclass = NULL;
        EquivalenceClass* ieclass = NULL;
        PathKey* pathkey = NULL;

        update_mergeclause_eclasses(root, rinfo);

        if (rinfo->outer_is_left) {
            oeclass = rinfo->left_ec;
            ieclass = rinfo->right_ec;
        } else {
            oeclass = rinfo->right_ec;
            ieclass = rinfo->left_ec;
        }

        /* outer eclass should match current or next pathkeys */
        /* we check this carefully for debugging reasons */
        if (oeclass != lastoeclass) {
            if (lop == NULL)
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                        (errmsg("too few pathkeys for mergeclauses when make inner pathkeys for merge"))));
            opathkey = (PathKey*)lfirst(lop);
            lop = lnext(lop);
            lastoeclass = opathkey->pk_eclass;
            if (oeclass != lastoeclass)
                ereport(ERROR,
                    (errmodule(MOD_OPT),
                        errcode(ERRCODE_OPTIMIZER_INCONSISTENT_STATE),
                        (errmsg("outer pathkeys do not match mergeclause when make inner pathkeys for merge"))));
        }

        /*
         * Often, we'll have same EC on both sides, in which case the outer
         * pathkey is also canonical for the inner side, and we can skip a
         * useless search.
         */
        if (ieclass == oeclass)
            pathkey = opathkey;
        else
            pathkey = make_canonical_pathkey(
                root, ieclass, opathkey->pk_opfamily, opathkey->pk_strategy, opathkey->pk_nulls_first);

        /*
         * Don't generate redundant pathkeys (which can happen if multiple
         * mergeclauses refer to the same EC).  Because we do this, the output
         * pathkey list isn't necessarily ordered like the mergeclauses, which
         * complicates life for create_mergejoin_plan().  But if we didn't,
         * we'd have a noncanonical sort key list, which would be bad; for one
         * reason, it certainly wouldn't match any available sort order for
         * the input relation.
         */
        if (!pathkey_is_redundant(pathkey, pathkeys))
            pathkeys = lappend(pathkeys, pathkey);
    }

    return pathkeys;
}

/*
 * trim_mergeclauses_for_inner_pathkeys
 *   This routine trims a list of mergeclauses to include just those that
 *   work with a specified ordering for the join's inner relation.
 *
 * 'mergeclauses' is a list of RestrictInfos for mergejoin clauses for the
 *         join relation being formed, in an order known to work for the
 *         currently-considered sort ordering of the join's outer rel.
 * 'pathkeys' is a pathkeys list showing the ordering of an inner-rel path;
 *         it should be equal to, or a truncation of, the result of
 *         make_inner_pathkeys_for_merge for these mergeclauses.
 *
 * What we return will be a prefix of the given mergeclauses list.
 *
 * We need this logic because make_inner_pathkeys_for_merge's result isn't
 * necessarily in the same order as the mergeclauses.  That means that if we
 * consider an inner-rel pathkey list that is a truncation of that result,
 * we might need to drop mergeclauses even though they match a surviving inner
 * pathkey.  This happens when they are to the right of a mergeclause that
 * matches a removed inner pathkey.
 *
 * The mergeclauses must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 */
List* trim_mergeclauses_for_inner_pathkeys(PlannerInfo* root, List* mergeclauses, List* pathkeys)
{
    List* new_mergeclauses = NIL;
    PathKey* pathkey = NULL;
    EquivalenceClass* pathkey_ec = NULL;
    bool matched_pathkey = false;
    ListCell* lip = NULL;
    ListCell* i = NULL;

    /* No pathkeys => no mergeclauses (though we don't expect this case) */
    if (pathkeys == NIL)
        return NIL;
    /* Initialize to consider first pathkey */
    lip = list_head(pathkeys);
    pathkey = (PathKey*)lfirst(lip);
    pathkey_ec = pathkey->pk_eclass;
    lip = lnext(lip);

    /* Scan mergeclauses to see how many we can use */
    foreach (i, mergeclauses) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(i);
        EquivalenceClass* clause_ec;

        /* Assume we needn't do update_mergeclause_eclasses again here */
        /* Check clause's inner-rel EC against current pathkey */
        clause_ec = rinfo->outer_is_left ? rinfo->right_ec : rinfo->left_ec;

        /* If we don't have a match, attempt to advance to next pathkey */
        if (clause_ec != pathkey_ec) {
            /* If we had no clauses matching this inner pathkey, must stop */
            if (!matched_pathkey)
                break;

            /* Advance to next inner pathkey, if any */
            if (lip == NULL)
                break;
            pathkey = (PathKey*)lfirst(lip);
            pathkey_ec = pathkey->pk_eclass;
            lip = lnext(lip);
            matched_pathkey = false;
        }

        /* If mergeclause matches current inner pathkey, we can use it */
        if (clause_ec == pathkey_ec) {
            new_mergeclauses = lappend(new_mergeclauses, rinfo);
            matched_pathkey = true;
        } else {
            /* Else, no hope of adding any more mergeclauses */
            break;
        }
    }

    return new_mergeclauses;
}

/****************************************************************************
 *		PATHKEY USEFULNESS CHECKS
 *
 * We only want to remember as many of the pathkeys of a path as have some
 * potential use, either for subsequent mergejoins or for meeting the query's
 * requested output ordering.  This ensures that add_path() won't consider
 * a path to have a usefully different ordering unless it really is useful.
 * These routines check for usefulness of given pathkeys.
 ****************************************************************************/
/*
 * pathkeys_useful_for_merging
 *		Count the number of pathkeys that may be useful for mergejoins
 *		above the given relation.
 *
 * We consider a pathkey potentially useful if it corresponds to the merge
 * ordering of either side of any joinclause for the rel.  This might be
 * overoptimistic, since joinclauses that require different other relations
 * might never be usable at the same time, but trying to be exact is likely
 * to be more trouble than it's worth.
 *
 * To avoid doubling the number of mergejoin paths considered, we would like
 * to consider only one of the two scan directions (ASC or DESC) as useful
 * for merging for any given target column.  The choice is arbitrary unless
 * one of the directions happens to match an ORDER BY key, in which case
 * that direction should be preferred, in hopes of avoiding a final sort step.
 * right_merge_direction() implements this heuristic.
 */
static int pathkeys_useful_for_merging(PlannerInfo* root, RelOptInfo* rel, List* pathkeys)
{
    int useful = 0;
    ListCell* i = NULL;

    foreach (i, pathkeys) {
        PathKey* pathkey = (PathKey*)lfirst(i);
        bool matched = false;
        ListCell* j = NULL;

        /* If "wrong" direction, not useful for merging */
        if (!right_merge_direction(root, pathkey))
            break;

        /*
         * First look into the EquivalenceClass of the pathkey, to see if
         * there are any members not yet joined to the rel.  If so, it's
         * surely possible to generate a mergejoin clause using them.
         */
        if (rel->has_eclass_joins && eclass_useful_for_merging(pathkey->pk_eclass, rel))
            matched = true;
        else {
            /*
             * Otherwise search the rel's joininfo list, which contains
             * non-EquivalenceClass-derivable join clauses that might
             * nonetheless be mergejoinable.
             */
            foreach (j, rel->joininfo) {
                RestrictInfo* restrictinfo = (RestrictInfo*)lfirst(j);

                if (restrictinfo->mergeopfamilies == NIL)
                    continue;
                update_mergeclause_eclasses(root, restrictinfo);

                if (pathkey->pk_eclass == restrictinfo->left_ec || pathkey->pk_eclass == restrictinfo->right_ec) {
                    matched = true;
                    break;
                }
            }
        }

        /*
         * If we didn't find a mergeclause, we're done --- any additional
         * sort-key positions in the pathkeys are useless.	(But we can still
         * mergejoin if we found at least one mergeclause.)
         */
        if (matched)
            useful++;
        else
            break;
    }

    return useful;
}

/*
 * right_merge_direction
 *		Check whether the pathkey embodies the preferred sort direction
 *		for merging its target column.
 */
static bool right_merge_direction(PlannerInfo* root, PathKey* pathkey)
{
    ListCell* l = NULL;

    foreach (l, root->query_pathkeys) {
        PathKey* query_pathkey = (PathKey*)lfirst(l);

        if (pathkey->pk_eclass == query_pathkey->pk_eclass &&
            OpFamilyEquals(pathkey->pk_opfamily, query_pathkey->pk_opfamily)) {
            /*
             * Found a matching query sort column.	Prefer this pathkey's
             * direction iff it matches.  Note that we ignore pk_nulls_first,
             * which means that a sort might be needed anyway ... but we still
             * want to prefer only one of the two possible directions, and we
             * might as well use this one.
             */
            return (pathkey->pk_strategy == query_pathkey->pk_strategy);
        }
    }

    /* If no matching ORDER BY request, prefer the ASC direction */
    return (pathkey->pk_strategy == BTLessStrategyNumber);
}

/*
 * pathkeys_useful_for_ordering
 *		Count the number of pathkeys that are useful for meeting the
 *		query's requested output ordering.
 *
 * Unlike merge pathkeys, this is an all-or-nothing affair: it does us
 * no good to order by just the first key(s) of the requested ordering.
 * So the result is always either 0 or list_length(root->query_pathkeys).
 */
static int pathkeys_useful_for_ordering(PlannerInfo* root, List* pathkeys)
{
    if (root->query_pathkeys == NIL)
        return 0; /* no special ordering requested */

    if (pathkeys == NIL)
        return 0; /* unordered path */

    if (pathkeys_contained_in(root->query_pathkeys, pathkeys)) {
        /* It's useful ... or at least the first N keys are */
        return list_length(root->query_pathkeys);
    }

    return 0; /* path ordering not useful */
}

/*
 * truncate_useless_pathkeys
 *		Shorten the given pathkey list to just the useful pathkeys.
 */
List* truncate_useless_pathkeys(PlannerInfo* root, RelOptInfo* rel, List* pathkeys)
{
    int nuseful;
    int nuseful2;

    nuseful = pathkeys_useful_for_merging(root, rel, pathkeys);
    nuseful2 = pathkeys_useful_for_ordering(root, pathkeys);
    if (nuseful2 > nuseful) {
        nuseful = nuseful2;
    }

    /*
     * Note: not safe to modify input list destructively, but we can avoid
     * copying the list if we're not actually going to change it
     */
    if (nuseful == 0)
        return NIL;
    else if (nuseful == list_length(pathkeys))
        return pathkeys;
    else
        return list_truncate(list_copy(pathkeys), nuseful);
}

/*
 * has_useful_pathkeys
 *		Detect whether the specified rel could have any pathkeys that are
 *		useful according to truncate_useless_pathkeys().
 *
 * This is a cheap test that lets us skip building pathkeys at all in very
 * simple queries.	It's OK to err in the direction of returning "true" when
 * there really aren't any usable pathkeys, but erring in the other direction
 * is bad --- so keep this in sync with the routines above!
 *
 * We could make the test more complex, for example checking to see if any of
 * the joinclauses are really mergejoinable, but that likely wouldn't win
 * often enough to repay the extra cycles.	Queries with neither a join nor
 * a sort are reasonably common, though, so this much work seems worthwhile.
 */
bool has_useful_pathkeys(PlannerInfo* root, RelOptInfo* rel)
{
    if (rel->joininfo != NIL || rel->has_eclass_joins)
        return true; /* might be able to use pathkeys for merging */
    if (root->query_pathkeys != NIL)
        return true; /* might be able to use them for ordering */
    return false;    /* definitely useless */
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
void
construct_pathkeys(PlannerInfo *root, List *tlist, List *activeWindows,
                   List *groupClause, bool canonical)
{
    Query      *parse = root->parse;

    /*
     * Calculate pathkeys that represent grouping/ordering requirements.
     * Stash them in PlannerInfo so that query_planner can canonicalize
     * them after EquivalenceClasses have been formed.	The sortClause is
     * certainly sort-able, but GROUP BY and DISTINCT might not be, in
     * which case we just leave their pathkeys empty.
     */

    /* To groupingSet, we need build it's groupPathKey according to it's lower levels sort clause.*/
    if (groupClause && grouping_is_sortable(groupClause)) {
        root->group_pathkeys = make_pathkeys_for_sortclauses(root, groupClause, tlist, canonical);
    } else {
        root->group_pathkeys = NIL;
    }

    /* We consider only the first (bottom) window in pathkeys logic */
    if (activeWindows != NIL) {
        WindowClause* wc = NULL;

        wc = (WindowClause*)linitial(activeWindows);

        root->window_pathkeys = make_pathkeys_for_window(root, wc, tlist, canonical);
    } else {
        root->window_pathkeys = NIL;
    }

    if (parse->distinctClause && grouping_is_sortable(parse->distinctClause)) {

        root->distinct_pathkeys = make_pathkeys_for_sortclauses(root,
                                    parse->distinctClause, tlist, canonical);
    } else {
        root->distinct_pathkeys = NIL;
    }

    root->sort_pathkeys = make_pathkeys_for_sortclauses(root, parse->sortClause, tlist, canonical);

    /* Remove the PARAM EC */
    if (canonical) {
        root->group_pathkeys = remove_param_pathkeys(root, root->group_pathkeys);
        root->window_pathkeys = remove_param_pathkeys(root, root->window_pathkeys);
        root->distinct_pathkeys = remove_param_pathkeys(root, root->distinct_pathkeys);
        root->sort_pathkeys = remove_param_pathkeys(root, root->sort_pathkeys);
    }

    /*
     * Figure out whether we want a sorted result from query_planner.
     *
     * If we have a sortable GROUP BY clause, then we want a result sorted
     * properly for grouping.  Otherwise, if we have window functions to
     * evaluate, we try to sort for the first window.  Otherwise, if
     * there's a sortable DISTINCT clause that's more rigorous than the
     * ORDER BY clause, we try to produce output that's sufficiently well
     * sorted for the DISTINCT.  Otherwise, if there is an ORDER BY
     * clause, we want to sort by the ORDER BY clause.
     *
     * Note: if we have both ORDER BY and GROUP BY, and ORDER BY is a
     * superset of GROUP BY, it would be tempting to request sort by ORDER
     * BY --- but that might just leave us failing to exploit an available
     * sort order at all.  Needs more thought.	The choice for DISTINCT
     * versus ORDER BY is much easier, since we know that the parser
     * ensured that one is a superset of the other.
     */
    if (root->group_pathkeys)
      root->query_pathkeys = root->group_pathkeys;
    else if (root->window_pathkeys)
      root->query_pathkeys = root->window_pathkeys;
    else if (list_length(root->distinct_pathkeys) > list_length(root->sort_pathkeys))
      root->query_pathkeys = root->distinct_pathkeys;
    else if (root->sort_pathkeys)
      root->query_pathkeys = root->sort_pathkeys;
    else
      root->query_pathkeys = NIL;

    return;
}

/*
 * Init the standard_qp_extra
 */
void standard_qp_init(PlannerInfo *root, void *extra, List *tlist, List *activeWindows, List *groupClause)
{
    if (!ENABLE_SQL_BETA_FEATURE(CANONICAL_PATHKEY)) {
        Assert (extra != NULL);
        standard_qp_extra *qp_extra = (standard_qp_extra *)extra;
        qp_extra->tlist = tlist;
        qp_extra->activeWindows = activeWindows;
        qp_extra->groupClause = groupClause;
    } else {
        construct_pathkeys(root, tlist, activeWindows, groupClause, false);
    }

    return;
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
void standard_qp_callback(PlannerInfo *root, void *extra)
{
    if (!ENABLE_SQL_BETA_FEATURE(CANONICAL_PATHKEY)) {
        Assert (extra != NULL);
        standard_qp_extra *qp_extra = (standard_qp_extra *)extra;
        construct_pathkeys(root, qp_extra->tlist, qp_extra->activeWindows,
                    qp_extra->groupClause, true);
    } else {
        root->group_pathkeys = canonicalize_pathkeys(root, root->group_pathkeys);
        root->window_pathkeys = canonicalize_pathkeys(root, root->window_pathkeys);
        root->distinct_pathkeys = canonicalize_pathkeys(root, root->distinct_pathkeys);
        root->sort_pathkeys = canonicalize_pathkeys(root, root->sort_pathkeys);
        root->query_pathkeys = canonicalize_pathkeys(root, root->query_pathkeys);
    }

    return;
}

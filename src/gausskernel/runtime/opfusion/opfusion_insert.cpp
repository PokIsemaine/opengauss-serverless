/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 *
 * opfusion_insert.cpp
 *        Definition of insert operator for bypass executor.
 *
 * IDENTIFICATION
 *        src/gausskernel/runtime/opfusion/opfusion_insert.cpp
 *
 * ---------------------------------------------------------------------------------------
 */

#include "opfusion/opfusion_insert.h"

#include "access/tableam.h"
#include "catalog/pg_partition_fn.h"
#include "catalog/storage_gtt.h"
#include "commands/matview.h"
#include "commands/sequence.h"
#include "executor/node/nodeModifyTable.h"
#include "parser/parse_coerce.h"
#include "utils/partitionmap.h"

void InsertFusion::InitGlobals()
{
    m_c_global = (InsertFusionGlobalVariable*)palloc0(sizeof(InsertFusionGlobalVariable));

    m_global->m_reloid = getrelid(linitial_int((List*)linitial(m_global->m_planstmt->resultRelations)),
                                  m_global->m_planstmt->rtable);
    ModifyTable* node = (ModifyTable*)m_global->m_planstmt->planTree;
    BaseResult* baseresult = (BaseResult*)linitial(node->plans);
    List* targetList = baseresult->plan.targetlist;

    Relation rel = heap_open(m_global->m_reloid, AccessShareLock);
    m_global->m_table_type = RelationIsUstoreFormat(rel) ? TAM_USTORE : TAM_HEAP;
    m_global->m_exec_func_ptr = (OpFusionExecfuncType)&InsertFusion::ExecInsert;

    m_global->m_natts = RelationGetDescr(rel)->natts;
    m_global->m_is_bucket_rel = RELATION_OWN_BUCKET(rel);
    m_global->m_tupDesc = CreateTupleDescCopy(RelationGetDescr(rel));
    m_global->m_tupDesc->td_tam_ops = GetTableAmRoutine(m_global->m_table_type);
    heap_close(rel, AccessShareLock);

    /* init param func const */
    m_global->m_paramNum = 0;
    m_global->m_paramLoc = (ParamLoc*)palloc0(m_global->m_natts * sizeof(ParamLoc));
    m_c_global->m_targetParamNum = 0;
    m_c_global->m_targetFuncNum = 0;
    m_c_global->m_targetFuncNodes = (FuncExprInfo*)palloc0(m_global->m_natts * sizeof(FuncExprInfo));
    m_c_global->m_targetConstNum = 0;
    m_c_global->m_targetConstLoc = (ConstLoc*)palloc0(m_global->m_natts * sizeof(ConstLoc));

    ListCell* lc = NULL;
    int i = 0;
    FuncExpr* func = NULL;
    TargetEntry* res = NULL;
    Expr* expr = NULL;
    OpExpr* opexpr = NULL;
    foreach (lc, targetList) {
        res = (TargetEntry*)lfirst(lc);
        expr = res->expr;
        Assert(
            IsA(expr, Const) || IsA(expr, Param) || IsA(expr, FuncExpr) || IsA(expr, RelabelType) || IsA(expr, OpExpr));
        while (IsA(expr, RelabelType)) {
            expr = ((RelabelType*)expr)->arg;
        }

        m_c_global->m_targetConstLoc[i].constLoc = -1;
        if (IsA(expr, FuncExpr)) {
            func = (FuncExpr*)expr;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].resno = res->resno;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].resname = res->resname;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].funcid = func->funcid;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].args = func->args;
            ++m_c_global->m_targetFuncNum;
        } else if (IsA(expr, Param)) {
            Param* param = (Param*)expr;
            m_global->m_paramLoc[m_c_global->m_targetParamNum].paramId = param->paramid;
            m_global->m_paramLoc[m_c_global->m_targetParamNum++].scanKeyIndx = i;
        } else if (IsA(expr, Const)) {
            Assert(IsA(expr, Const));
            m_c_global->m_targetConstLoc[i].constValue = ((Const*)expr)->constvalue;
            m_c_global->m_targetConstLoc[i].constIsNull = ((Const*)expr)->constisnull;
            m_c_global->m_targetConstLoc[i].constLoc = i;
        } else if (IsA(expr, OpExpr)) {
            opexpr = (OpExpr*)expr;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].resno = res->resno;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].resname = res->resname;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].funcid = opexpr->opfuncid;
            m_c_global->m_targetFuncNodes[m_c_global->m_targetFuncNum].args = opexpr->args;
            ++m_c_global->m_targetFuncNum;
        }
        i++;
    }
    m_c_global->m_targetConstNum = i;

}
void InsertFusion::InitLocals(ParamListInfo params)
{
    m_c_local.m_estate = CreateExecutorStateForOpfusion(m_local.m_localContext, m_local.m_tmpContext);
    m_c_local.m_estate->es_range_table = m_global->m_planstmt->rtable;
    m_c_local.m_estate->es_plannedstmt = m_global->m_planstmt;
    m_local.m_reslot = MakeSingleTupleTableSlot(m_global->m_tupDesc);
    if (m_global->m_table_type == TAM_USTORE) {
        m_local.m_reslot->tts_tam_ops = TableAmUstore;
    }
    m_local.m_values = (Datum*)palloc0(m_global->m_natts * sizeof(Datum));
    m_local.m_isnull = (bool*)palloc0(m_global->m_natts * sizeof(bool));
    m_c_local.m_curVarValue = (Datum*)palloc0(m_global->m_natts * sizeof(Datum));
    m_c_local.m_curVarIsnull = (bool*)palloc0(m_global->m_natts * sizeof(bool));
    initParams(params);
    m_local.m_receiver = NULL;
    m_local.m_isInsideRec = true;
    m_local.m_optype = INSERT_FUSION;
}

InsertFusion::InsertFusion(MemoryContext context, CachedPlanSource* psrc, List* plantree_list, ParamListInfo params)
    : OpFusion(context, psrc, plantree_list)
{
    MemoryContext old_context = NULL;
    if (!IsGlobal()) {
        old_context = MemoryContextSwitchTo(m_global->m_context);
        InitGlobals();
        MemoryContextSwitchTo(old_context);
    } else {
        m_c_global = ((InsertFusion*)(psrc->opFusionObj))->m_c_global;
    }
    old_context = MemoryContextSwitchTo(m_local.m_localContext);
    InitLocals(params);
    MemoryContextSwitchTo(old_context);
}

void InsertFusion::refreshParameterIfNecessary()
{
    ParamListInfo parms = m_local.m_outParams != NULL ? m_local.m_outParams : m_local.m_params;
    bool func_isnull = false;
    /* save cur var value */
    for (int i = 0; i < m_global->m_tupDesc->natts; i++) {
        m_c_local.m_curVarValue[i] = m_local.m_values[i];
        m_c_local.m_curVarIsnull[i] = m_local.m_isnull[i];
    }
    /* refresh const value */
    for (int i = 0; i < m_c_global->m_targetConstNum; i++) {
        if (m_c_global->m_targetConstLoc[i].constLoc >= 0) {
            m_local.m_values[m_c_global->m_targetConstLoc[i].constLoc] = m_c_global->m_targetConstLoc[i].constValue;
            m_local.m_isnull[m_c_global->m_targetConstLoc[i].constLoc] = m_c_global->m_targetConstLoc[i].constIsNull;
        }
    }
    /* calculate func result */
    for (int i = 0; i < m_c_global->m_targetFuncNum; ++i) {
        ELOG_FIELD_NAME_START(m_c_global->m_targetFuncNodes[i].resname);
        if (m_c_global->m_targetFuncNodes[i].funcid != InvalidOid) {
            func_isnull = false;
            m_local.m_values[m_c_global->m_targetFuncNodes[i].resno - 1] =
                CalFuncNodeVal(m_c_global->m_targetFuncNodes[i].funcid,
                               m_c_global->m_targetFuncNodes[i].args,
                               &func_isnull,
                               m_c_local.m_curVarValue,
                               m_c_local.m_curVarIsnull);
            m_local.m_isnull[m_c_global->m_targetFuncNodes[i].resno - 1] = func_isnull;
        }
        ELOG_FIELD_NAME_END;
    }
    /* mapping params */
    if (m_c_global->m_targetParamNum > 0) {
        for (int i = 0; i < m_c_global->m_targetParamNum; i++) {
            m_local.m_values[m_global->m_paramLoc[i].scanKeyIndx] =
                parms->params[m_global->m_paramLoc[i].paramId - 1].value;
            m_local.m_isnull[m_global->m_paramLoc[i].scanKeyIndx] =
                parms->params[m_global->m_paramLoc[i].paramId - 1].isnull;
        }
    }
}

extern HeapTuple searchPgPartitionByParentIdCopy(char parttype, Oid parentId);
PartKeyExprResult ComputePartKeyExprTuple(Relation rel, EState *estate, TupleTableSlot *slot, Relation partRel, char* partExprKeyStr)
{
    Relation pgPartition = NULL;
    HeapTuple partitionedTuple = NULL;
    bool isnull = false;
    Datum newval = 0;
    Node* partkeyexpr = NULL;
    Relation tmpRel = NULL;
    if (partExprKeyStr && pg_strcasecmp(partExprKeyStr, "") != 0) {
        partkeyexpr = (Node*)stringToNode_skip_extern_fields(partExprKeyStr);                      
    } else {
        ereport(ERROR, (errcode(ERRCODE_PARTITION_ERROR), errmsg("The partition expr key can't be null for table %s", NameStr(rel->rd_rel->relname))));
    }
    (void)lockNextvalWalker(partkeyexpr, NULL);
    ExprState *exprstate = ExecPrepareExpr((Expr *)partkeyexpr, estate);
    ExprContext *econtext;
    econtext = GetPerTupleExprContext(estate);
    econtext->ecxt_scantuple = slot;
    isnull = false;
    newval = ExecEvalExpr(exprstate, econtext, &isnull, NULL);
    Const** boundary = NULL;
    if (PointerIsValid(partRel))
        tmpRel = partRel;
    else
        tmpRel = rel;

    if (tmpRel->partMap->type == PART_TYPE_RANGE)
        boundary = ((RangePartitionMap*)(tmpRel->partMap))->rangeElements[0].boundary;
    else if (tmpRel->partMap->type == PART_TYPE_LIST)
        boundary = ((ListPartitionMap*)(tmpRel->partMap))->listElements[0].boundary[0].values;
    else if (tmpRel->partMap->type == PART_TYPE_HASH)
        boundary = ((HashPartitionMap*)(tmpRel->partMap))->hashElements[0].boundary;
    else
        ereport(ERROR, (errcode(ERRCODE_PARTITION_ERROR), errmsg("Unsupported partition type : %d", tmpRel->partMap->type)));

    if (!isnull)
        newval = datumCopy(newval, boundary[0]->constbyval, boundary[0]->constlen);

    return {newval, isnull};
}

static void ExecReleaseResource(Tuple tuple, TupleTableSlot *slot, ResultRelInfo *result_rel_info, EState *estate,
                                Relation bucket_rel, Relation rel, Partition part, Relation partRel)
{
    tableam_tops_free_tuple(tuple);
    (void)ExecClearTuple(slot);
    ExecCloseIndices(result_rel_info);
    ExecDoneStepInFusion(estate);
    if (bucket_rel != NULL) {
        bucketCloseRelation(bucket_rel);
    }
    if (RELATION_IS_PARTITIONED(rel) && part != NULL) {
        partitionClose(rel, part, RowExclusiveLock);
        releaseDummyRelation(&partRel);
    }
}


unsigned long InsertFusion::ExecInsert(Relation rel, ResultRelInfo* result_rel_info)
{
    /*******************
     * step 1: prepare *
     *******************/
    Relation bucket_rel = NULL;
    int2 bucketid = InvalidBktId;
    Oid partOid = InvalidOid;
    Partition part = NULL;
    Relation partRel = NULL;
    bool rel_isblockchain = rel->rd_isblockchain;
    CommandId mycid = GetCurrentCommandId(true);
    init_gtt_storage(CMD_INSERT, result_rel_info);

    /************************
     * step 2: begin insert *
     ************************/
    Tuple tuple = tableam_tops_form_tuple(m_global->m_tupDesc, m_local.m_values,
        m_local.m_isnull, rel->rd_tam_ops);
    Assert(tuple != NULL);
    if (RELATION_IS_PARTITIONED(rel)) {
        m_c_local.m_estate->esfRelations = NULL;
        int partitionno = INVALID_PARTITION_NO;
        m_local.m_reslot->tts_tuple = tuple;
        partOid = getPartitionIdFromTuple(rel, tuple, m_c_local.m_estate, m_local.m_reslot, &partitionno, false, m_c_local.m_estate->es_plannedstmt->hasIgnore);
        if (m_c_local.m_estate->es_plannedstmt->hasIgnore && partOid == InvalidOid) {
            ExecReleaseResource(tuple, m_local.m_reslot, result_rel_info, m_c_local.m_estate, bucket_rel, rel, part,
                                partRel);
            return 0;
        }
        part = PartitionOpenWithPartitionno(rel, partOid, partitionno, RowExclusiveLock);
        partRel = partitionGetRelation(rel, part);
    }

    if (m_global->m_is_bucket_rel) {
        bucketid = computeTupleBucketId(result_rel_info->ri_RelationDesc, (HeapTuple)tuple);
        bucket_rel = InitBucketRelation(bucketid, rel, part);
    }

    (void)ExecStoreTuple(tuple, m_local.m_reslot, InvalidBuffer, false);

    /*
     * Compute stored generated columns
     */
    if (result_rel_info->ri_RelationDesc->rd_att->constr &&
        result_rel_info->ri_RelationDesc->rd_att->constr->has_generated_stored) {
        ExecComputeStoredGenerated(result_rel_info, m_c_local.m_estate, m_local.m_reslot, tuple, CMD_INSERT);
        if (tuple != m_local.m_reslot->tts_tuple) {
            tableam_tops_free_tuple(tuple);
            tuple = m_local.m_reslot->tts_tuple;
        }
    }

    if (rel->rd_att->constr) {
        /*
         * If values violate constraints, directly return.
         */
        if(!ExecConstraints(result_rel_info, m_local.m_reslot, m_c_local.m_estate, true)) {
            if (u_sess->utils_cxt.sql_ignore_strategy_val != SQL_OVERWRITE_NULL) {
                ExecReleaseResource(tuple, m_local.m_reslot, result_rel_info, m_c_local.m_estate, bucket_rel, rel, part,
                                    partRel);
                return 0;
            }
            tuple = ReplaceTupleNullCol(RelationGetDescr(result_rel_info->ri_RelationDesc), m_local.m_reslot);
            /* Double check constraints in case that new val in column with not null constraints
             * violated check constraints */
            ExecConstraints(result_rel_info, m_local.m_reslot, m_c_local.m_estate, true);
        }
        tuple = ExecAutoIncrement(rel, m_c_local.m_estate, m_local.m_reslot, tuple);
        if (tuple != m_local.m_reslot->tts_tuple) {
            tableam_tops_free_tuple(tuple);
            tuple = m_local.m_reslot->tts_tuple;
        }
    }
    Relation destRel = RELATION_IS_PARTITIONED(rel) ? partRel : rel;
    Relation target_rel = (bucket_rel == NULL) ? destRel : bucket_rel;
    if (rel_isblockchain && (!RelationIsUstoreFormat(rel))) {
        HeapTuple tmp_tuple = (HeapTuple)tuple;
        MemoryContext old_context = MemoryContextSwitchTo(m_local.m_tmpContext);
        tuple = set_user_tuple_hash(tmp_tuple, target_rel, NULL);
        (void)ExecStoreTuple(tuple, m_local.m_reslot, InvalidBuffer, false);
        m_local.m_ledger_hash_exist = hist_table_record_insert(target_rel, (HeapTuple)tuple, &m_local.m_ledger_relhash);
        (void)MemoryContextSwitchTo(old_context);
        tableam_tops_free_tuple(tmp_tuple);
    }

    /* check unique constraint first if SQL has keyword IGNORE */
    bool isgpi = false;
    ConflictInfoData conflictInfo;
    Oid conflictPartOid = InvalidOid;
    int2 conflictBucketid = InvalidBktId;
    if (m_c_local.m_estate->es_plannedstmt && m_c_local.m_estate->es_plannedstmt->hasIgnore &&
        !ExecCheckIndexConstraints(m_local.m_reslot, m_c_local.m_estate, target_rel, part, &isgpi, bucketid,
                                   &conflictInfo, &conflictPartOid, &conflictBucketid)) {
        ereport(WARNING, (errmsg("duplicate key value violates unique constraint in table \"%s\"",
                                 RelationGetRelationName(target_rel))));
        ExecReleaseResource(tuple, m_local.m_reslot, result_rel_info, m_c_local.m_estate, bucket_rel, rel, part,
                            partRel);
        return 0;
    }

    (void)tableam_tuple_insert(bucket_rel == NULL ? destRel : bucket_rel, tuple, mycid, 0, NULL);
    if (!RELATION_IS_PARTITIONED(rel)) {
        /* try to insert tuple into mlog-table. */
        if (rel != NULL && rel->rd_mlogoid != InvalidOid) {
            /* judge whether need to insert into mlog-table */
            HeapTuple htuple = NULL;
            if (rel->rd_tam_ops == TableAmUstore) {
                htuple = UHeapToHeap(rel->rd_att, (UHeapTuple)tuple);
                insert_into_mlog_table(rel, rel->rd_mlogoid, htuple, &htuple->t_self,
                    GetCurrentTransactionId(), 'I');
            } else {
                insert_into_mlog_table(rel, rel->rd_mlogoid, (HeapTuple)tuple, &((HeapTuple)tuple)->t_self,
                    GetCurrentTransactionId(), 'I');
            }
        }
    }

    /* insert index entries for tuple */
    List* recheck_indexes = NIL;
    if (result_rel_info->ri_NumIndices > 0) {
        recheck_indexes = ExecInsertIndexTuples(m_local.m_reslot,
                                                &(((HeapTuple)tuple)->t_self),
                                                m_c_local.m_estate,
                                                RELATION_IS_PARTITIONED(rel) ? partRel : NULL,
                                                RELATION_IS_PARTITIONED(rel) ? part : NULL,
                                                bucketid, NULL, NULL);
    }

    list_free_ext(recheck_indexes);

    if (result_rel_info->ri_WithCheckOptions != NIL)
        ExecWithCheckOptions(result_rel_info, m_local.m_reslot, m_c_local.m_estate);

    /****************
     * step 3: done *
     ****************/
    ExecReleaseResource(tuple, m_local.m_reslot, result_rel_info, m_c_local.m_estate, bucket_rel, rel, part, partRel);

    return 1;
}

bool InsertFusion::execute(long max_rows, char* completionTag)
{
    bool success = false;
    errno_t errorno = EOK;

    /*******************
     * step 1: prepare *
     *******************/
    Relation rel = heap_open(m_global->m_reloid, RowExclusiveLock);

    ResultRelInfo* result_rel_info = makeNode(ResultRelInfo);
    InitResultRelInfo(result_rel_info, rel, 1, 0);

    if (result_rel_info->ri_RelationDesc->rd_rel->relhasindex) {
        ExecOpenIndices(result_rel_info, true);
    }

    init_gtt_storage(CMD_INSERT, result_rel_info);
    m_c_local.m_estate->es_result_relation_info = result_rel_info;
    m_c_local.m_estate->es_plannedstmt = m_global->m_planstmt;
    refreshParameterIfNecessary();

    ModifyTable* node = (ModifyTable*)(m_global->m_planstmt->planTree);
    PlanState* ps = NULL;
    if (node->withCheckOptionLists != NIL) {
        Plan* plan = (Plan*)linitial(node->plans);
        ps = ExecInitNode(plan, m_c_local.m_estate, 0);
        List* wcoList = (List*)linitial(node->withCheckOptionLists);
        List* wcoExprs = NIL;
        ListCell* ll = NULL;

        foreach(ll, wcoList) {
            WithCheckOption* wco = (WithCheckOption*)lfirst(ll);
            ExprState* wcoExpr = ExecInitExpr((Expr*)wco->qual, ps);
            wcoExprs = lappend(wcoExprs, wcoExpr);
        }

        result_rel_info->ri_WithCheckOptions = wcoList;
        result_rel_info->ri_WithCheckOptionExprs = wcoExprs;
    }

    /************************
     * step 2: begin insert *
     ************************/

    unsigned long nprocessed = (this->*(m_global->m_exec_func_ptr))(rel, result_rel_info);
    heap_close(rel, RowExclusiveLock);

    /****************
     * step 3: done *
     ****************/
    if (ps != NULL) {
        ExecEndNode(ps);
    }
    success = true;
    m_local.m_isCompleted = true;
    if (m_local.m_ledger_hash_exist && !IsConnFromApp()) {
        errorno = snprintf_s(completionTag, COMPLETION_TAG_BUFSIZE, COMPLETION_TAG_BUFSIZE - 1,
            "INSERT 0 %ld %lu\0", nprocessed, m_local.m_ledger_relhash);
    } else {
        errorno =
            snprintf_s(completionTag, COMPLETION_TAG_BUFSIZE, COMPLETION_TAG_BUFSIZE - 1, "INSERT 0 %ld", nprocessed);
    }
    securec_check_ss(errorno, "\0", "\0");
    FreeExecutorStateForOpfusion(m_c_local.m_estate);
    u_sess->statement_cxt.current_row_count = nprocessed;
    u_sess->statement_cxt.last_row_count = u_sess->statement_cxt.current_row_count;
    return success;
}

#ifndef SERIALIZEPLAN_CPP
#include "SerializePlan.h"  


void assign_listcell_from_cpp(const ListCell* cpp_cell, proto_plan::ListCell* proto_cell) {
    if (cpp_cell == nullptr) {
        return;
    }

    // 根据实际类型赋值
    if (cpp_cell->data.ptr_value) {
        proto_cell->set_ptr_value(static_cast<const char*>(cpp_cell->data.ptr_value), sizeof(cpp_cell->data.ptr_value));
    } else if (cpp_cell->data.int_value) {
        proto_cell->set_int_value(cpp_cell->data.int_value);
    } else if (cpp_cell->data.oid_value) {
        proto_cell->set_oid_value(cpp_cell->data.oid_value);
    }

    // 递归处理下一个节点
    if (cpp_cell->next) {
        proto_plan::ListCell* next_proto_cell = proto_cell->mutable_next(); // 获取下一个节点的 Protobuf 消息
        assign_listcell_from_cpp(cpp_cell->next, next_proto_cell);
    }
}

void assign_list_from_cpp(const List* cpp_list, proto_plan::List* proto_list) {
    proto_list->set_type(static_cast<proto_plan::NodeTag>(cpp_list->type)); // Assuming NodeTag is an enum
    proto_list->set_length(cpp_list->length);

    // Handle head
    if (cpp_list->head != nullptr) {
       proto_plan::ListCell* proto_head = proto_list->mutable_head();
        assign_listcell_from_cpp(cpp_list->head, proto_head);
    }

    // Handle tail
    if (cpp_list->tail != nullptr) {
        proto_plan::ListCell* proto_tail = proto_list->mutable_tail();
        assign_listcell_from_cpp(cpp_list->tail, proto_tail);
    }
}

void assign_bitmapset_from_cpp(const Bitmapset* cpp_bitmapset, proto_plan::Bitmapset* proto_bitmapset) {
    if (!cpp_bitmapset || !proto_bitmapset) {
        return; // 检查指针有效性
    }

    // 赋值 nwords
    proto_bitmapset->set_nwords(cpp_bitmapset->nwords);

    // 将 words 复制到 protobuf 消息中
    for (int i = 0; i < cpp_bitmapset->nwords; ++i) {
        proto_bitmapset->add_words(cpp_bitmapset->words[i]);
    }
}

void assign_distribution_from_cpp(const Distribution& cpp_distribution, proto_plan::Distribution* proto_distribution) {
    // 假设 Distribution 有其他字段需要赋值
    proto_distribution->set_group_oid(cpp_distribution.group_oid);
    assign_bitmapset_from_cpp(cpp_distribution.bms_data_nodeids, proto_distribution->mutable_bms_data_nodeids());
    
}

void assign_cursor_data_from_cpp(const Cursor_Data* cpp_cursor_data, proto_plan::Cursor_Data* proto_cursor_data) {
    if (!cpp_cursor_data || !proto_cursor_data) {
        return; // 检查指针有效性
    }

    // 赋值各字段
    proto_cursor_data->set_row_count(cpp_cursor_data->row_count);
    proto_cursor_data->set_cur_dno(cpp_cursor_data->cur_dno);
    proto_cursor_data->set_is_open(cpp_cursor_data->is_open);
    proto_cursor_data->set_found(cpp_cursor_data->found);
    proto_cursor_data->set_not_found(cpp_cursor_data->not_found);
    proto_cursor_data->set_null_open(cpp_cursor_data->null_open);
    proto_cursor_data->set_null_fetch(cpp_cursor_data->null_fetch);
}

void assign_expr_from_cpp(const Expr* cpp_expr, proto_plan::Expr* proto_expr) {
    if (!cpp_expr || !proto_expr) {
        return; // 检查指针有效性
    }

    proto_expr->set_type(static_cast<proto_plan::NodeTag>(cpp_expr->type)); // 假设 NodeTag 可以转换为 int32
    proto_expr->set_selec(cpp_expr->selec);
}

void assign_const_from_cpp(const Const* cpp_const, proto_plan::Const* proto_const) {
    if (!cpp_const || !proto_const) {
        return; // 检查指针有效性
    }

    // 假设已经有函数来赋值 Expr
    assign_expr_from_cpp(&cpp_const->xpr, proto_const->mutable_xpr());

    proto_const->set_consttype(cpp_const->consttype);
    proto_const->set_consttypmod(cpp_const->consttypmod);
    proto_const->set_constcollid(cpp_const->constcollid);
    proto_const->set_constlen(cpp_const->constlen);
    
    proto_const->set_constvalue(cpp_const->constvalue);

    proto_const->set_constisnull(cpp_const->constisnull);
    proto_const->set_constbyval(cpp_const->constbyval);
    proto_const->set_location(cpp_const->location);
    proto_const->set_ismaxvalue(cpp_const->ismaxvalue);

    // 赋值 Cursor_Data
    assign_cursor_data_from_cpp(&cpp_const->cursor_data, proto_const->mutable_cursor_data());
}

void assign_slice_boundary_from_cpp(const SliceBoundary* cpp_slice, proto_plan::SliceBoundary* proto_slice) {
    if (!cpp_slice || !proto_slice) {
        return; // 检查指针有效性
    }

    proto_slice->set_type(static_cast<proto_plan::NodeTag>(cpp_slice->type)); // 转换 NodeTag
    proto_slice->set_nodeidx(cpp_slice->nodeIdx);
    proto_slice->set_len(cpp_slice->len);

    // 处理边界数组
    for (int i = 0; i < RANGE_PARTKEYMAXNUM; ++i) {
        if (cpp_slice->boundary[i]) { // 检查指针有效性
            proto_plan::Const* proto_boundary = proto_slice->add_boundary();
            assign_const_from_cpp(cpp_slice->boundary[i], proto_boundary);
        }
    }
}

void assign_exec_boundary_from_cpp(const ExecBoundary* cpp_exec_boundary, proto_plan::ExecBoundary* proto_exec_boundary) {
    if (!cpp_exec_boundary || !proto_exec_boundary) {
        return; // 检查指针有效性
    }

    proto_exec_boundary->set_type(static_cast<proto_plan::NodeTag>(cpp_exec_boundary->type)); // 转换 NodeTag
    proto_exec_boundary->set_locatortype(std::string(1, cpp_exec_boundary->locatorType)); // 转换为 string
    proto_exec_boundary->set_count(cpp_exec_boundary->count);

    // 处理切片边界数组
    for (int i = 0; cpp_exec_boundary->eles[i] != nullptr; ++i) {
        proto_plan::SliceBoundary* proto_slice = proto_exec_boundary->add_eles();
        assign_slice_boundary_from_cpp(cpp_exec_boundary->eles[i], proto_slice);
    }
}

// 假设有其他辅助函数，如将 List、Bitmapset、ExecNodes 等转换为 Protobuf 对象
void assign_exec_nodes_from_cpp(const ExecNodes* cpp_exec_nodes, proto_plan::ExecNodes* proto_exec_nodes) {
    if (!proto_exec_nodes) {
        return;
    }
    
    // 假设 ExecNodes 结构有这些字段
    proto_exec_nodes->set_type(static_cast<proto_plan::NodeTag>(cpp_exec_nodes->type));
    proto_exec_nodes->set_accesstype(static_cast<proto_plan::RelationAccessType>(cpp_exec_nodes->accesstype));
    proto_exec_nodes->set_baselocatortype(std::string(1, cpp_exec_nodes->baselocatortype));
    proto_exec_nodes->set_en_relid(cpp_exec_nodes->en_relid);
    proto_exec_nodes->set_rangelistoid(cpp_exec_nodes->rangelistOid);
    proto_exec_nodes->set_need_range_prune(cpp_exec_nodes->need_range_prune);
    proto_exec_nodes->set_en_varno(cpp_exec_nodes->en_varno);
    proto_exec_nodes->set_bucketmapidx(cpp_exec_nodes->bucketmapIdx);
    proto_exec_nodes->set_nodelist_is_nil(cpp_exec_nodes->nodelist_is_nil);
    proto_exec_nodes->set_bucketid(cpp_exec_nodes->bucketid);
    proto_exec_nodes->set_bucketrelid(cpp_exec_nodes->bucketrelid);

    assign_list_from_cpp(cpp_exec_nodes->primarynodelist, proto_exec_nodes->mutable_primarynodelist());
    assign_list_from_cpp(cpp_exec_nodes->nodeList, proto_exec_nodes->mutable_nodelist());
    assign_list_from_cpp(cpp_exec_nodes->en_expr, proto_exec_nodes->mutable_en_expr());
    assign_list_from_cpp(cpp_exec_nodes->en_dist_vars, proto_exec_nodes->mutable_en_dist_vars());
    assign_list_from_cpp(cpp_exec_nodes->original_nodeList, proto_exec_nodes->mutable_original_nodelist());
    assign_list_from_cpp(cpp_exec_nodes->dynamic_en_expr, proto_exec_nodes->mutable_dynamic_en_expr());
    assign_list_from_cpp(cpp_exec_nodes->bucketexpr, proto_exec_nodes->mutable_bucketexpr());
    assign_list_from_cpp(cpp_exec_nodes->hotkeys, proto_exec_nodes->mutable_hotkeys());

    assign_distribution_from_cpp(cpp_exec_nodes->distribution, proto_exec_nodes->mutable_distribution());
    assign_exec_boundary_from_cpp(cpp_exec_nodes->boundaries, proto_exec_nodes->mutable_boundaries());

}

void assign_right_ref_state_from_cpp(const RightRefState* cpp_right_ref_state, proto_plan::RightRefState* proto_right_ref_state) {
    if (!cpp_right_ref_state) {
        return; // 检查指针有效性
    }

    proto_right_ref_state->set_issupported(cpp_right_ref_state->isSupported);
    proto_right_ref_state->set_isinserthasrightref(cpp_right_ref_state->isInsertHasRightRef);
    proto_right_ref_state->set_explicitattrlen(cpp_right_ref_state->explicitAttrLen);
    
    // 赋值 explicitAttrNos 数组
    // 赋值 explicitAttrNos 数组
    for (int i = 0; i < cpp_right_ref_state->explicitAttrLen; ++i) {
        proto_right_ref_state->add_explicitattrnos(cpp_right_ref_state->explicitAttrNos[i]);
    }

    // 赋值 constValues 数组
    for (int i = 0; i < cpp_right_ref_state->colCnt; ++i) {
        proto_plan::Const* proto_const = proto_right_ref_state->add_constvalues();
        assign_const_from_cpp(cpp_right_ref_state->constValues[i], proto_const);
    }

    proto_right_ref_state->set_colcnt(cpp_right_ref_state->colCnt);

    // 赋值 values 数组
    for (int i = 0; i < cpp_right_ref_state->colCnt; ++i) {
        proto_right_ref_state->add_values(static_cast<uint64_t>(cpp_right_ref_state->values[i])); // 确保数据类型转换
    }

    // 赋值 hasExecs 数组
    for (int i = 0; i < cpp_right_ref_state->colCnt; ++i) {
        proto_right_ref_state->add_hasexecs(cpp_right_ref_state->hasExecs[i]);
    }

    // 赋值 isNulls 数组
    for (int i = 0; i < cpp_right_ref_state->colCnt; ++i) {
        proto_right_ref_state->add_isnulls(cpp_right_ref_state->isNulls[i]);
    }

    proto_right_ref_state->set_isupsert(cpp_right_ref_state->isUpsert);
    proto_right_ref_state->set_isupserthasrightref(cpp_right_ref_state->isUpsertHasRightRef);
    proto_right_ref_state->set_usexplicitattrlen(cpp_right_ref_state->usExplicitAttrLen);

    // 赋值 usExplicitAttrNos 数组
    for (int i = 0; i < cpp_right_ref_state->usExplicitAttrLen; ++i) {
        proto_right_ref_state->add_usexplicitattrnos(cpp_right_ref_state->usExplicitAttrNos[i]);
    }
}

// 递归地将 C++ 版 Plan 赋值给 Protobuf 版 Plan
void assign_plan_from_cpp(const Plan* cpp_plan, proto_plan::Plan* proto_plan) {
    if (!proto_plan || !cpp_plan) {
        return; // 检查指针有效性
    }

    proto_plan->set_plan_node_id(cpp_plan->plan_node_id);
    proto_plan->set_parent_node_id(cpp_plan->parent_node_id);
    proto_plan->set_exec_type(static_cast<proto_plan::RemoteQueryExecType>(cpp_plan->exec_type));

    proto_plan->set_startup_cost(cpp_plan->startup_cost);
    proto_plan->set_total_cost(cpp_plan->total_cost);
    proto_plan->set_plan_rows(cpp_plan->plan_rows);
    proto_plan->set_multiple(cpp_plan->multiple);
    proto_plan->set_plan_width(cpp_plan->plan_width);
    proto_plan->set_dop(cpp_plan->dop);

    proto_plan->set_pred_rows(cpp_plan->pred_rows);
    proto_plan->set_pred_startup_time(cpp_plan->pred_startup_time);
    proto_plan->set_pred_total_time(cpp_plan->pred_total_time);
    proto_plan->set_pred_max_memory(cpp_plan->pred_max_memory);

    proto_plan->set_recursive_union_plan_nodeid(cpp_plan->recursive_union_plan_nodeid);
    proto_plan->set_recursive_union_controller(cpp_plan->recursive_union_controller);
    proto_plan->set_control_plan_nodeid(cpp_plan->control_plan_nodeid);
    proto_plan->set_is_sync_plannode(cpp_plan->is_sync_plannode);

    // 处理 targetlist
    assign_list_from_cpp(cpp_plan->targetlist, proto_plan->mutable_targetlist());
    assign_list_from_cpp(cpp_plan->qual, proto_plan->mutable_qual());
    assign_list_from_cpp(cpp_plan->initPlan, proto_plan->mutable_initplan());
    assign_list_from_cpp(cpp_plan->distributed_keys, proto_plan->mutable_distributed_keys());
    assign_list_from_cpp(cpp_plan->var_list, proto_plan->mutable_var_list());
    assign_list_from_cpp(cpp_plan->filterIndexList, proto_plan->mutable_filterindexlist());
    assign_list_from_cpp(cpp_plan->flatList, proto_plan->mutable_flatlist());

    // 处理 lefttree 和 righttree
    if (cpp_plan->lefttree) {
        assign_plan_from_cpp(cpp_plan->lefttree, proto_plan->mutable_lefttree());
    }

    if (cpp_plan->righttree) {
        assign_plan_from_cpp(cpp_plan->righttree, proto_plan->mutable_righttree());
    }

    proto_plan->set_ispwj(cpp_plan->ispwj);
    proto_plan->set_paramno(cpp_plan->paramno);
    proto_plan->set_subparamno(cpp_plan->subparamno);

    // 处理 exec_nodes
    if (cpp_plan->exec_nodes) {
        assign_exec_nodes_from_cpp(cpp_plan->exec_nodes, proto_plan->mutable_exec_nodes());
    }

    // 处理 extParam 和 allParam
    if (cpp_plan->extParam) {
        assign_bitmapset_from_cpp(cpp_plan->extParam, proto_plan->mutable_extparam());
    }

    if (cpp_plan->allParam) {
        assign_bitmapset_from_cpp(cpp_plan->allParam, proto_plan->mutable_allparam());
    }

    proto_plan->set_vec_output(cpp_plan->vec_output);
    proto_plan->set_hasuniqueresults(cpp_plan->hasUniqueResults);
    proto_plan->set_isdeltatable(cpp_plan->isDeltaTable);

    // 处理 operatorMemKB
    for (int i = 0; i < 2; ++i) {
        proto_plan->add_operatormemkb(cpp_plan->operatorMemKB[i]);
    }

    proto_plan->set_operatormaxmem(cpp_plan->operatorMaxMem);
    proto_plan->set_parallel_enabled(cpp_plan->parallel_enabled);
    proto_plan->set_hashashfilter(cpp_plan->hasHashFilter);

    // 处理 ng_operatorMemKBArray
    for (int i = 0; i < cpp_plan->ng_num; ++i) {
        for (int j = 0; j < 2; ++j) {
            proto_plan->add_ng_operatormemkbarray(cpp_plan->ng_operatorMemKBArray[i][j]);
        }
    }

    proto_plan->set_ng_num(cpp_plan->ng_num);
    proto_plan->set_innerdistinct(cpp_plan->innerdistinct);
    proto_plan->set_outerdistinct(cpp_plan->outerdistinct);

    // 处理 rightRefState
    if (cpp_plan->rightRefState) {
        assign_right_ref_state_from_cpp(cpp_plan->rightRefState, proto_plan->mutable_rightrefstate());
    }
}
#endif
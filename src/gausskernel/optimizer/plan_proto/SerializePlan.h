#ifndef SERIALIZEPLAN_HPP
#define SERIALIZEPLAN_HPP

#include "query_plan.pb.h"  // Protobuf 生成的头文件
#include "nodes/plannodes.h"

// 将 C++ 中的 ListCell 赋值到 Protobuf 中的 ListCell
void assign_listcell_from_cpp(const ListCell* cpp_cell, proto_plan::ListCell* proto_cell);

// 将 C++ 中的 List 赋值到 Protobuf 中的 List
void assign_list_from_cpp(const List* cpp_list, proto_plan::List* proto_list);

// 将 C++ 中的 Bitmapset 赋值到 Protobuf 中的 Bitmapset
void assign_bitmapset_from_cpp(const Bitmapset* cpp_bitmapset, proto_plan::Bitmapset* proto_bitmapset);

// 将 C++ 中的 Distribution 赋值到 Protobuf 中的 Distribution
void assign_distribution_from_cpp(const Distribution& cpp_distribution, proto_plan::Distribution* proto_distribution);

// 将 C++ 中的 Cursor_Data 赋值到 Protobuf 中的 Cursor_Data
void assign_cursor_data_from_cpp(const Cursor_Data* cpp_cursor_data, proto_plan::Cursor_Data* proto_cursor_data);

// 将 C++ 中的 Expr 赋值到 Protobuf 中的 Expr
void assign_expr_from_cpp(const Expr* cpp_expr, proto_plan::Expr* proto_expr);

// 将 C++ 中的 Const 赋值到 Protobuf 中的 Const
void assign_const_from_cpp(const Const* cpp_const, proto_plan::Const* proto_const);

// 将 C++ 中的 SliceBoundary 赋值到 Protobuf 中的 SliceBoundary
void assign_slice_boundary_from_cpp(const SliceBoundary* cpp_slice, proto_plan::SliceBoundary* proto_slice);

// 将 C++ 中的 ExecBoundary 赋值到 Protobuf 中的 ExecBoundary
void assign_exec_boundary_from_cpp(const ExecBoundary* cpp_exec_boundary, proto_plan::ExecBoundary* proto_exec_boundary);

// 将 C++ 中的 ExecNodes 赋值到 Protobuf 中的 ExecNodes
void assign_exec_nodes_from_cpp(const ExecNodes* cpp_exec_nodes, proto_plan::ExecNodes* proto_exec_nodes);

// 将 C++ 中的 RightRefState 赋值到 Protobuf 中的 RightRefState
void assign_right_ref_state_from_cpp(const RightRefState* cpp_right_ref_state, proto_plan::RightRefState* proto_right_ref_state);

// 将 C++ 中的 Plan 递归地赋值到 Protobuf 中的 Plan
void assign_plan_from_cpp(const Plan* cpp_plan, proto_plan::Plan* proto_plan);

#endif  // SERIALIZEPLAN_HPP

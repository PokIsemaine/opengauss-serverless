
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
 * ss_common_attr.h
 * 
 * IDENTIFICATION
 *        src/include/ddes/dms/ss_common_attr.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef SS_COMMON_ATTR_H
#define SS_COMMON_ATTR_H

#ifndef OPENGAUSS
#define OPENGAUSS
#endif

#include "dms_api.h"
#include "ss_init.h"

#ifdef ENABLE_LITE_MODE
#define ENABLE_DMS false
#define ENABLE_REFORM false
#define ENABLE_VERIFY_PAGE_VERSION false
#else
#define ENABLE_DMS (g_instance.attr.attr_storage.dms_attr.enable_dms && !IsInitdb)
#define ENABLE_REFORM (g_instance.attr.attr_storage.dms_attr.enable_reform)
#define ENABLE_VERIFY_PAGE_VERSION (g_instance.attr.attr_storage.dms_attr.enable_verify_page)
#endif

#define SS_REFORM_REFORMER                                                  \
    (ENABLE_DMS && (g_instance.dms_cxt.SSReformInfo.in_reform == true) \
    && (g_instance.dms_cxt.SSReformInfo.dms_role == DMS_ROLE_REFORMER))

#define SS_REFORM_PARTNER                                                   \
    (ENABLE_DMS && (g_instance.dms_cxt.SSReformInfo.in_reform == true) \
    && (g_instance.dms_cxt.SSReformInfo.dms_role != DMS_ROLE_REFORMER))

#define SS_NORMAL_PRIMARY                                                  \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState == NODESTATE_NORMAL) \
    && (g_instance.dms_cxt.SSReformerControl.primaryInstId == SS_MY_INST_ID) \
    && (g_instance.dms_cxt.SSReformInfo.in_reform == false))

#define SS_NORMAL_STANDBY                                                  \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState == NODESTATE_NORMAL) \
    && (g_instance.dms_cxt.SSReformerControl.primaryInstId != SS_MY_INST_ID) \
    && (g_instance.dms_cxt.SSReformInfo.in_reform == false))

#define SS_PRIMARY_MODE (SS_NORMAL_PRIMARY || SS_REFORM_REFORMER)

#define SS_STANDBY_MODE (SS_NORMAL_STANDBY || SS_REFORM_PARTNER)

#define SS_IN_REFORM (ENABLE_DMS && g_instance.dms_cxt.SSReformInfo.in_reform == true)

#define SS_IN_FLUSHCOPY (ENABLE_DMS && g_instance.dms_cxt.SSRecoveryInfo.in_flushcopy == true)

#define SS_STANDBY_FAILOVER (g_instance.dms_cxt.SSClusterState == NODESTATE_STANDBY_FAILOVER_PROMOTING)

#define SS_PRIMARY_NORMAL_REFORM \
    (SS_REFORM_REFORMER && (g_instance.dms_cxt.SSReformInfo.reform_type == DMS_REFORM_TYPE_FOR_NORMAL_OPENGAUSS))

#define SS_PERFORMING_SWITCHOVER \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState > NODESTATE_NORMAL && \
    g_instance.dms_cxt.SSClusterState != NODESTATE_STANDBY_FAILOVER_PROMOTING))

#define SS_STANDBY_PROMOTING \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState == NODESTATE_STANDBY_PROMOTING))

#define SS_PRIMARY_DEMOTING                                                             \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState >= NODESTATE_PRIMARY_DEMOTING) && \
    (g_instance.dms_cxt.SSClusterState <= NODESTATE_PROMOTE_APPROVE))

#define SS_PRIMARY_DEMOTED \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState == NODESTATE_PROMOTE_APPROVE))

#define SS_STANDBY_WAITING                                                             \
    (ENABLE_DMS && (g_instance.dms_cxt.SSClusterState == NODESTATE_STANDBY_WAITING || \
    g_instance.dms_cxt.SSClusterState == NODESTATE_STANDBY_REDIRECT))

/* Mode in dorado hyperreplication and dms enabled as follow */

/* main standby in standby cluster */
#define SS_STANDBY_CLUSTER_MAIN_STANDBY                                    \
    (ENABLE_DMS && (t_thrd.xlog_cxt.server_mode == STANDBY_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  STANDBY_MODE) && \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_STANDBY) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* standby mode in primary or standby cluster */
#define SS_PRIMARY_STANDBY_CLUSTER_STANDBY                                    \
    (ENABLE_DMS && (t_thrd.xlog_cxt.server_mode == NORMAL_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  NORMAL_MODE) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* standby mode in primary cluster */
#define SS_PRIMARY_CLUSTER_STANDBY                                    \
    (ENABLE_DMS && (t_thrd.xlog_cxt.server_mode == NORMAL_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  NORMAL_MODE) && \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_PRIMARY) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* arbitrary mode when dorado hyperreplication and dms enabled */
#define SS_PRIMARY_STANDBY_CLUSTER_NORMAL                                   \
    (ENABLE_DMS && ((g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_PRIMARY) || \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_STANDBY)) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* primary mode in primary cluster, after reform done and primary id has been determined */
#define SS_PRIMARY_CLUSTER_NORMAL_PRIMARY                                    \
    (SS_NORMAL_PRIMARY && (t_thrd.xlog_cxt.server_mode == PRIMARY_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  PRIMARY_MODE) && \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_PRIMARY) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* main standby in standby cluster, after reform done and primary id has been determined */
#define SS_STANDBY_CLUSTER_NORMAL_MAIN_STANDBY                                    \
    (SS_NORMAL_PRIMARY && (t_thrd.xlog_cxt.server_mode == STANDBY_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  STANDBY_MODE) && \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_STANDBY) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* standby mode in standby cluster, after reform done and primary id has been determined */
#define SS_STANDBY_CLUSTER_NORMAL_STANDBY                                    \
    (SS_NORMAL_STANDBY && (t_thrd.xlog_cxt.server_mode == STANDBY_MODE || \
    t_thrd.postmaster_cxt.HaShmData->current_mode ==  STANDBY_MODE) && \
    (g_instance.attr.attr_common.cluster_run_mode == RUN_MODE_STANDBY) && \
    (g_instance.attr.attr_storage.xlog_file_path != 0))

/* standby mode in primary or standby, after reform done and primary id has been determined */
#define SS_PRIMARY_STANDBY_CLUSTER_NORMAL_STANDBY                                    \
    (SS_NORMAL_STANDBY && (g_instance.attr.attr_storage.xlog_file_path != 0))

#define SS_CLUSTER_ONDEMAND_NOT_NORAML \
    (ENABLE_DMS && (g_instance.dms_cxt.SSRecoveryInfo.cluster_ondemand_status != CLUSTER_NORMAL))
#define SS_CLUSTER_ONDEMAND_BUILD \
    (ENABLE_DMS && (g_instance.dms_cxt.SSRecoveryInfo.cluster_ondemand_status == CLUSTER_IN_ONDEMAND_BUILD))
#define SS_CLUSTER_ONDEMAND_RECOVERY \
    (ENABLE_DMS && (g_instance.dms_cxt.SSRecoveryInfo.cluster_ondemand_status == CLUSTER_IN_ONDEMAND_REDO))
#define SS_CLUSTER_ONDEMAND_NORMAL \
    (ENABLE_DMS && (g_instance.dms_cxt.SSRecoveryInfo.cluster_ondemand_status == CLUSTER_NORMAL))
#define SS_STANDBY_ONDEMAND_BUILD (SS_STANDBY_MODE && SS_CLUSTER_ONDEMAND_BUILD)
#define SS_STANDBY_ONDEMAND_RECOVERY (SS_STANDBY_MODE && SS_CLUSTER_ONDEMAND_RECOVERY)
#define SS_STANDBY_ONDEMAND_NORMAL (SS_STANDBY_MODE && SS_CLUSTER_ONDEMAND_NORMAL)
#define SS_STANDBY_ONDEMAND_NOT_NORMAL (SS_STANDBY_MODE && SS_CLUSTER_ONDEMAND_NOT_NORAML)

/* DMS_BUF_NEED_LOAD */
#define BUF_NEED_LOAD           0x1
/* DMS_BUF_IS_LOADED */
#define BUF_IS_LOADED           0x2
/* DMS_BUF_LOAD_FAILED */
#define BUF_LOAD_FAILED         0x4
/* DMS_BUF_NEED_TRANSFER */
#define BUF_NEED_TRANSFER       0x8
/* mark buffer whether is extended when dms read from disk */
#define BUF_IS_EXTEND           0x10

/* mark buffer whether is persistent when dms read from disk, don't clear */
#define BUF_IS_RELPERSISTENT    0x20
#define BUF_IS_RELPERSISTENT_TEMP    0x40
#define BUF_READ_MODE_ZERO_LOCK    0x80
#define BUF_DIRTY_NEED_FLUSH    0x100
#define BUF_ERTO_NEED_MARK_DIRTY    0x200

#define SS_BROADCAST_FAILED_RETRYCOUNTS 4
#define SS_BROADCAST_WAIT_INFINITE (0xFFFFFFFF)
#define SS_BROADCAST_WAIT_FIVE_SECONDS (5000)
#define SS_BROADCAST_WAIT_ONE_SECOND (1000)
#define SS_BROADCAST_WAIT_FIVE_MICROSECONDS (5)

#define SS_ACQUIRE_LOCK_DO_NOT_WAIT 0
#define SS_ACQUIRE_LOCK_RETRY_INTERVAL (50)   // 50ms

#define DMS_MSG_MAX_WAIT_TIME (10 * 1000) // 10s
#define SS_REFORM_WAIT_TIME (5000) // 5ms

/* length of segment filename like '/1' */
#define SEG_MAINFORK_FILENAME_LEN   2
/* length of segment vm filename like '1_vm' */
#define SEG_VMFORK_FILENAME_LEN     4
/* length of segment fsm filename like '1_fsm' */
#define SEG_FSMFORK_FILENAME_LEN    5

/* max length of max int range as char */
#define MAX_LEN_OF_MAXINTRANGE      12

typedef enum SSBroadcastOp {
    BCAST_CANCEL_TRX_FOR_SWITCHOVER = 0,
    BCAST_SI,
    BCAST_SEGDROPTL,
    BCAST_DROP_REL_ALL_BUFFER,
    BCAST_DROP_REL_RANGE_BUFFER,
    BCAST_DROP_DB_ALL_BUFFER,
    BCAST_DROP_SEG_SPACE,
    BCAST_CANCEL_TRX_FOR_FAILOVER,
    BCAST_DDLLOCK,
    BCAST_DDLLOCKRELEASE,
    BCAST_DDLLOCKRELEASE_ALL,
    BCAST_CHECK_DB_BACKENDS,
    BCAST_RELOAD_REFORM_CTRL_PAGE,
    BCAST_END
} SSBroadcastOp;

typedef enum SSBroadcastOpAck {
    BCAST_CANCEL_TRX_ACK = 0,
    BCAST_CHECK_DB_BACKENDS_ACK,
    BCAST_ACK_END
} SSBroadcastOpAck;

typedef struct SSBroadcastCmdOnly {
    SSBroadcastOp type; // must be first
} SSBroadcastCmdOnly;

typedef enum SSReformType {
    DMS_REFORM_TYPE_FOR_NORMAL = 0,
    DMS_REFORM_TYPE_FOR_BUILD,
    DMS_REFORM_TYPE_FOR_FAILOVER,
    DMS_REFORM_TYPE_FOR_SWITCHOVER,
    DMS_REFORM_TYPE_FOR_NORMAL_OPENGAUSS,
    DMS_REFORM_TYPE_FOR_FAILOVER_OPENGAUSS,
    DMS_REFORM_TYPE_FOR_SWITCHOVER_OPENGAUSS,
    DMS_REFORM_TYPE_FOR_FULL_CLEAN,
    DMS_REFORM_TYPE_FOR_MAINTAIN
} SSReformType;

typedef enum SSGlobalClusterState {
    CLUSTER_IN_ONDEMAND_BUILD = 0,
    CLUSTER_IN_ONDEMAND_REDO,
    CLUSTER_NORMAL
} SSGlobalClusterState;

typedef enum SSOndemandRequestRedoStatus {
    ONDEMAND_REDO_DONE = 0,
    ONDEMAND_REDO_SKIP,
    ONDEMAND_REDO_FAIL,
    ONDEMAND_REDO_TIMEOUT
} SSOndemandRequestRedoStatus;


#endif

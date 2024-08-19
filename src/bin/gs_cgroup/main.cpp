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
 *-------------------------------------------------------------------------
 *
 * main.cpp
 * main function file for gs_cgroup utility
 *
 * IDENTIFICATION
 * src/bin/gs_cgroup/main.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "gs_cgroup.h"

int main(int argc, char** argv)
{
    char* cpuset = NULL;
    int ret = 0;

    if (argc < 2) {
        usage();
        exit(-1);
    }

    // log output redirect
    init_log(PROG_NAME);

    /* print the log about arguments of gs_cgroup */
    char arguments[MAX_BUF_SIZE] = {0x00};
    for (int i = 0; i < argc; i++) {
        errno_t rc = strcat_s(arguments, MAX_BUF_SIZE, argv[i]);
        size_t len = strlen(arguments);
        if (rc != EOK || len >= (MAX_BUF_SIZE - 2))
            break;
        arguments[len] = ' ';
        arguments[len + 1] = '\0';
    }
    write_log("The gs_cgroup run with the following arguments: [%s].\n", arguments);

    /* get the cpu count value */
    cgutil_cpucnt = gsutil_get_cpu_count();

    if (cgutil_cpucnt == -1) {
        fprintf(stderr,
            "get cpu core range failed, please check if \"/proc/cpuinfo\""
            " or \"/sys/devices/system\" is acceptable. \n");
        exit(-1);
    }

    int rc = sprintf_s(cgutil_allset, sizeof(cgutil_allset), "%d-%d", 0, cgutil_cpucnt - 1);
    securec_check_intval(rc, , -1);

    /* parse the options */
    ret = parse_options(argc, argv);
    if (-1 == ret) {
        fprintf(stderr, "HINT: please run 'gs_cgroup -h' to display the usage!\n");
        exit(-1);
    }

    if (cgutil_version != NULL) {
        fprintf(stdout, "gs_cgroup %s\n", cgutil_version);
        return 0;
    }

    if (geteuid() == 0 && cgutil_opt.mflag) {
        cgexec_mount_cgroups();
    }        

    if (geteuid() == 0 && cgutil_opt.umflag && !cgutil_opt.dflag) {
        cgexec_umount_cgroups();
        exit(0);
    }

    /* retrieve the information of configure file; if it doesn't, create one */
    if (initialize_cgroup_config() == -1)
        return -1;

    /* check upgrade flag */
    if (cgutil_opt.upgrade) {
        cgutil_opt.refresh = 1;

        /* maybe we need not do upgrade */
        if (cgexec_check_mount_for_upgrade() == -1) {
            goto error;
        }
    }

    if (cgutil_opt.cflag) {
        /* run as root user */
        if (geteuid() == 0 && cgutil_opt.upgrade == 0) {
            /* check if cgroups have been mounted if it doesn't specify mflag */
            if (-1 == (ret = cgexec_mount_root_cgroup())) {
                goto error;
            }
        }
    }

    /* initialize libcgroup */
    ret = cgroup_init();
    if (ret) {
        fprintf(stderr,
            "FATAL: libcgroup initialization failed: %s\n"
            "please run 'gs_cgroup -m' to "
            "mount cgroup by root user!\n",
            cgroup_strerror(ret));
        goto error;
    }

    /* get memory set */
    if (-1 == cgexec_get_cgroup_cpuset_info(TOPCG_ROOT, &cpuset)) {
        fprintf(stderr, "ERROR: failed to get cpusets and mems during initialization.\n");
        goto error;
    }

    rc = snprintf_s(cgutil_allset, CPUSET_LEN, CPUSET_LEN - 1, "%s", cpuset);
    securec_check_intval(rc, , -1);
    free(cpuset);
    cpuset = NULL;

    /* create/delete/update operation */
    if (cgutil_opt.cflag) {
        if (cgexec_create_groups() == -1) {
            goto error;
        }
    } else if (cgutil_opt.dflag) {
        if (cgexec_drop_groups() == -1) {
            goto error;
        }
    } else if (cgutil_opt.uflag) {
        if (cgexec_update_groups() == -1) {
            goto error;
        }
    } else if (cgutil_opt.revert) {
        if (cgexec_revert_groups() == -1) {
            goto error;
        }
    }

    /* refresh current groups */
    if (cgutil_opt.refresh) {
        if (cgexec_refresh_groups() == -1) {
            goto error;
        }
    }

    /* recover the last changes of groups */
    if (cgutil_opt.recover) {
        if (cgexec_recover_groups() == -1) {
            goto error;
        }
    }

    /* process the exceptional data */
    if (*cgutil_opt.edata && *cgutil_opt.clsname && -1 == cgexcp_class_exception())
        goto error;

    /* display the cgroup configuration file information */
    if (cgutil_opt.display)
        cgconf_display_groups();

    /* display the cgroup tree information */
    if (cgutil_opt.ptree) {
        if (cgptree_display_cgroups() == -1) {
            goto error;
        }
    }

    if (cgutil_vaddr[0] != NULL)
        (void)munmap(cgutil_vaddr[0], GSCGROUP_ALLNUM * sizeof(gscgroup_grp_t));

    return 0;
error:
    if (cgutil_vaddr[0] != NULL)
        (void)munmap(cgutil_vaddr[0], GSCGROUP_ALLNUM * sizeof(gscgroup_grp_t));
    write_log("gs_cgroup execution error.\n");
    exit(-1);
}
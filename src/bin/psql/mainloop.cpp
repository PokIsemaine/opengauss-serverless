/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2012, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.c
 */
#include "settings.h"
#include "postgres_fe.h"
#include "mainloop.h"

#include "command.h"
#include "common.h"
#include "input.h"

#include "mb/pg_wchar.h"
#include <cxxabi.h>
#ifdef ENABLE_UT
#define static
#endif

#if defined(USE_ASSERT_CHECKING) || defined(FASTCHECK)
bool isSlashEnd(const char* strLine)
{
    if (strLine == NULL) {
        return NULL;
    }
    const char* pStr = strLine;
    while ('\0' != *pStr) {
        if (' ' == *pStr || '\t' == *pStr || '\r' == *pStr || '\n' == *pStr) {
            pStr++;
            continue;
        } else if ('/' == *pStr) {
            ++pStr;
            while ('\0' != *pStr) {
                if (' ' != *pStr && '\t' != *pStr && '\r' != *pStr && '\n' != *pStr) {
                    return false;
                }
                pStr++;
            }
            return true;
        } else
            return false;
    }
    return false;
}

const char* const SPLITSTR = "\n----Formatter End----\n";
#endif

static void GetSessionTimeout(char* resbuf, size_t buflen)
{
    PGresult* StRes = NULL;
    int rt = 0;
    /* Get current value of session_timeout. */
    StRes = PQexec(pset.db, "show session_timeout");
    if (StRes == NULL) {
        psql_error("Failed to get session_timeout, "
                   "and no error details received from backend. Please check the log files of backend.\n");
        exit(EXIT_FAILURE);
    }

    if (PGRES_COMMAND_OK != PQresultStatus(StRes) && PGRES_TUPLES_OK != PQresultStatus(StRes)) {
        psql_error("Failed to get session_timeout: %s\n", PQresultErrorMessage(StRes));
        exit(EXIT_FAILURE);
    }

    if (PQgetvalue(StRes, 0, 0) == NULL) {
        psql_error("Failed to get session_timeout: result is empty.\n");
        exit(EXIT_FAILURE);
    }

    rt = strncpy_s(resbuf, buflen, PQgetvalue(StRes, 0, 0), buflen - 1);
    check_strncpy_s(rt);

    PQclear(StRes);
}

static void SetSessionTimeout(const char* session_timeout)
{
    PGresult* StRes = NULL;
    char setQuery[100] = "";

    check_sprintf_s(sprintf_s(setQuery, sizeof(setQuery), "set session_timeout = '%s'", session_timeout));

    StRes = PQexec(pset.db, setQuery);
    if (StRes == NULL) {
        psql_error("Failed to set session_timeout "
                   "and no error details received from backend. Please check the log files of backend.\n");
        PQclear(StRes);
        exit(EXIT_FAILURE);
    }

    if (PGRES_COMMAND_OK != PQresultStatus(StRes) && PGRES_TUPLES_OK != PQresultStatus(StRes)) {
        psql_error("Failed to set session_timeout: %s\n", PQresultErrorMessage(StRes));
        PQclear(StRes);
        exit(EXIT_FAILURE);
    }

    PQclear(StRes);
}

static void JudgeEndStateInBFormat(const char* inputLine, bool &is_b_format, char* delimiter_name, bool is_new_lines)
{
    /* Convert inputLine to lowercase */ 
    char *inputLine_temp = pg_strdup(inputLine);
    inputLine_temp = pg_strtolower(inputLine_temp);

    /* Determine whether the command is a delimiter command, and if so, save the result. */
    static bool is_just_one_check = false;
    static bool is_just_two_check = false;
    PGresult* res = NULL ;
    char *tokenPtr = strstr(inputLine_temp, "delimiter");
    char *tokenPtr1 = strstr(inputLine_temp, "\\c" );
    errno_t rc = 0;
    
    if (!is_just_one_check) {
        res = PQexec(pset.db, "show sql_compatibility");
        if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK) {
            is_b_format = strcmp (PQgetvalue(res, 0, 0), "B") == 0;
        }   
        PQclear(res);
        res = NULL;
        is_just_one_check = true;
    }

    if (tokenPtr1 != NULL) {
        is_just_one_check = false;
    }

    if (is_b_format) {
        if (!is_just_two_check && is_new_lines) {
            res = PQexec(pset.db, "show delimiter_name");
            if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK) {
                rc = strcpy_s(delimiter_name, DELIMITER_LENGTH, PQgetvalue(res, 0, 0));
                securec_check_c(rc, "\0", "\0"); 
            }
            PQclear(res);
            res = NULL;
            is_just_two_check = true;
        }
    } else if (strcmp(delimiter_name,";") != 0) {
        rc = strcpy_s(delimiter_name, DELIMITER_LENGTH, ";");
        securec_check_c(rc, "\0", "\0"); 
    }

    if (tokenPtr != NULL || tokenPtr1 != NULL) {
        is_just_two_check = false;
    }

    free(inputLine_temp);
    inputLine_temp =NULL;
}

static bool is_match_delimiter_name(const char* left, const char* right)
{
    if (strlen(left) < strlen(right)) {
        return false;
    }
    while (*right) {
        if (*left++ != *right++) {
            return false;
        }
    }
    return true ;
}

static char* get_correct_str(char*str, const char *delimiter_name, bool is_new_lines)
{
    /* Determine whether it is a delimiter command. */
    char *str_temp = pg_strdup(str);
    str_temp = pg_strtolower(str_temp);
    bool is_delimiter = false;
    char *token = strstr(str_temp, "delimiter");
    errno_t rc = 0;
    char *end = NULL;
    bool quoted = false;
    char quoted_type = 0;

    if(token != NULL) {
        is_delimiter = true;
        char* pos = str_temp;
        while(pos != token) {
            if(*pos == ' ') {
                pos++;
            } else {
                is_delimiter = false;
                break;
            }
        }
        if(is_delimiter) {
            end = pos + strlen("delimiter");
            if(*end != ' ' && *end != '\0') {
                is_delimiter = false;
            }
        }
    }
    if (is_new_lines && is_delimiter) {
        /* delimiter command, looking for the first parameter */
        Size deliSlen = strlen(str) + strlen(delimiter_name) + DELIMITER_LENGTH;
        char *deliResultTemp = (char *)pg_malloc(deliSlen);
        char *start = deliResultTemp;
        int length = end - str_temp;
        char *temp_pos = str;
        bool is_spec_type = false;
        while(length--) {
            *start++ = *temp_pos++;
        }
        *start++ = ' ';
        while(*temp_pos == ' ') {
            temp_pos++;
        }
        if (*temp_pos != '\0') {
            if (*temp_pos != ';') {
                if (JudgeQuteType(*temp_pos)) {
                    quoted_type = *temp_pos;
                    *start++ = *temp_pos++;
                    quoted = true;
                }
                if (JudgeSpecialType(*temp_pos)) {
                    is_spec_type = true;
                    *start++ = *temp_pos++;
                }
                for (; *temp_pos; temp_pos++) {
                    bool is_spec = JudgeSpecialType(*temp_pos) ? true : false;
                    if (!quoted && ((is_spec_type ^ is_spec) || *temp_pos == ';'))
                        break;
                    *start++ = *temp_pos;
                    if ((!quoted && *temp_pos == ' ') || (quoted && *temp_pos == quoted_type)) 
                        break;
                }
            } else {
                *start++ = *temp_pos++;
            }
        }

        *start = '\0';
        char* deliResult = (char *) pg_malloc(deliSlen);
        rc = sprintf_s(deliResult, deliSlen, "%s \"%s\"", deliResultTemp, delimiter_name);
        securec_check_ss_c(rc, "", ""); 
        free(str_temp);
        str_temp =NULL;
        free(deliResultTemp);
        deliResultTemp =NULL;
        return deliResult;
    } 
    free(str_temp);
    str_temp =NULL;

    if (!JudgeAlphType(*delimiter_name)) {
        return pg_strdup(str);
    }
    Size slen = 2 * strlen(str) + 1;
    char* result = (char *) pg_malloc(slen + 2);
    char* temp = result;
    char* pos;
    char* end_of_str = str + strlen(str);
    char special_str = 0;
    char in;
    for (pos = str; pos < end_of_str; pos++) {
        in = *pos;
        if (!special_str && is_match_delimiter_name(pos , delimiter_name)) {
            *temp++ =' ';
            int delimiter_name_length = strlen(delimiter_name);
            while ( delimiter_name_length > 0 && *pos != '\0') {
                *temp++ = *pos++;
                delimiter_name_length--;
            }
            pos--;
            *temp++ = ' ';
        } else {
            if (in == special_str) {
                 special_str = 0;
            } else if (!special_str && JudgeQuteType(in)) {
                special_str = (char)in; 
            }
            *temp++ = *pos;
        }
    }   
    *temp = '\0';
    return result;
}

/*
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 */
int MainLoop(FILE* source, char* querystring)
{
    PsqlScanState scan_state = NULL;   /* lexer working state */
    volatile PQExpBuffer query_buf;    /* buffer for query being accumulated */
    volatile PQExpBuffer previous_buf; /* if there isn't anything in the new
                                        * buffer yet, use this one for \e,
                                        * etc. */
    PQExpBuffer history_buf;           /* earlier lines of a multi-line command, not
                                        * yet saved to readline history */
    char* line = NULL;                 /* current line of input */
    size_t added_nl_pos;
    bool success = false;
    bool line_saved_in_history = false;;
    volatile int successResult = EXIT_SUCCESS;
    volatile backslashResult slashCmdStatus = PSQL_CMD_UNKNOWN;
    volatile promptStatus_t prompt_status = PROMPT_READY;
    volatile int count_eof = 0;
    volatile bool die_on_error = false;
    bool isonlyparse = false;
    /* Save the prior command source */
    FILE* prev_cmd_source = NULL;
    bool prev_cmd_interactive = false;
    uint64 prev_lineno;

    /* Save old settings */
    prev_cmd_source = pset.cur_cmd_source;
    prev_cmd_interactive = pset.cur_cmd_interactive;
    prev_lineno = pset.lineno;

    /* show where source from,file or string */
    bool source_flag = true;

    /* Save the stmts and counts info in parallel execute mode. */
    int query_count = 0;
    char** query_stmts = NULL;
    bool is_b_format = false;
    char delimiter_name[DELIMITER_LENGTH]=";";
    char *line_temp = NULL;

    errno_t rc = 0;

    /* Establish new source */
    if (NULL != source) {
        pset.cur_cmd_source = source;
        pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);
    }
    pset.lineno = 0;

    if (pset.cur_cmd_interactive) {
        const char* val = GetVariable(pset.vars, "HISTSIZE");
        if (val != NULL) {
            setHistSize("HISTSIZE", val, false);
        }
    }

    /* Create working state */
    scan_state = psql_scan_create();

    query_buf = createPQExpBuffer();
    previous_buf = createPQExpBuffer();
    history_buf = createPQExpBuffer();
    if (PQExpBufferBroken(query_buf) || PQExpBufferBroken(previous_buf) || PQExpBufferBroken(history_buf)) {
        psql_error("out of memory\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize current database compatibility */
    PGresult* res = PQexec(pset.db, "show sql_compatibility");
    if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK) {
        is_b_format = strcmp (PQgetvalue(res, 0, 0), "B") == 0;
    }
    PQclear(res);
    res = NULL;

    /* main loop to get queries and execute them */
    while (successResult == EXIT_SUCCESS) {
        /*
         * Clean up after a previous Control-C
         */
        if (cancel_pressed) {
            if (!pset.cur_cmd_interactive) {
                /*
                 * You get here if you stopped a script with Ctrl-C.
                 */
                successResult = EXIT_USER;
                break;
            }

            cancel_pressed = false;
        }

        /*
         * Establish longjmp destination for exiting from wait-for-input. We
         * must re-do this each time through the loop for safety, since the
         * jmpbuf might get changed during command execution.
         */
        if (sigsetjmp(sigint_interrupt_jmp, 1) != 0) {
            /* got here with longjmp */

            /* reset parsing state */
            psql_scan_finish(scan_state);
            psql_scan_reset(scan_state);
            resetPQExpBuffer(query_buf);
            resetPQExpBuffer(history_buf);
            count_eof = 0;
            slashCmdStatus = PSQL_CMD_UNKNOWN;
            prompt_status = PROMPT_READY;
            cancel_pressed = false;

            if (pset.cur_cmd_interactive) {
                (void)putc('\n', stdout);
            } else {
                successResult = EXIT_USER;
                break;
            }
        }

        (void)fflush(stdout);

        /*
         * get another line
         */
        if (pset.cur_cmd_interactive) {
            /* May need to reset prompt, eg after \r command */
            if (query_buf->len == 0) {
                prompt_status = PROMPT_READY;
            }
            line = gets_interactive(get_prompt(prompt_status));
        } else {
            if (NULL != source) {
                /* fgets on SUSE12 may raise a buffer currupt of source->_IO_read_base.
                 * So, here we specify a buffer to the file, and it will be ok.
                 */
                char file_buffer[BUFSIZ] = {'\0'};
                if (source != stdin) {
                    setbuffer(source, file_buffer, sizeof(file_buffer) - 1);
                }
                line = gets_fromFile(source);
                //std::cout<<line << std::endl;
            } else {
                /* only assign line the first time in querystring situation. */
                if (source_flag) {
                    line = querystring;
                    source_flag = false;
                } else {
                    line = NULL;
                }
            }
            if (source_flag && (line == NULL) && ferror(source)) {
                successResult = EXIT_FAILURE;
            }
        }

        /*
         * query_buf holds query already accumulated.  line is the malloc'd
         * new line of input (note it must be freed before looping around!)
         */

        /* No more input.  Time to quit, or \i done */
        if (line == NULL) {
            if (pset.cur_cmd_interactive) {
                /* This tries to mimic bash's IGNOREEOF feature. */
                count_eof++;

                if (count_eof < GetVariableNum(pset.vars, "IGNOREEOF", 0, 10, false)) {
                    if (!pset.quiet) {
                        printf(_("Use \"\\q\" to leave %s.\n"), pset.progname);
                    }
                    continue;
                }

                puts(pset.quiet ? "" : "\\q");
            }
            break;
        }

        count_eof = 0;

        pset.lineno++;

        /* ignore UTF-8 Unicode byte-order mark */
        if (pset.lineno == 1 && pset.encoding == PG_UTF8 && strncmp(line, "\xef\xbb\xbf", 3) == 0) {
            rc = memmove_s(line, strlen(line + 3) + 1, line + 3, strlen(line + 3) + 1);
            check_memmove_s(rc);
        }

        /* nothing left on line? then ignore */
        if (source_flag && line[0] == '\0' && !psql_scan_in_quote(scan_state)) {
            free(line);
            line = NULL;
            continue;
        }

        /* A request for help? Be friendly and give them some guidance */
        if (pset.cur_cmd_interactive && query_buf->len == 0 && pg_strncasecmp(line, "help", 4) == 0 &&
            (line[4] == '\0' || line[4] == ';' || isspace((unsigned char)line[4]))) {
            free(line);
            line = NULL;
            (void)puts(_("You are using gsql, the command-line interface to gaussdb."));
            printf(_("Type:  \\copyright for distribution terms\n"
                     "       \\h for help with SQL commands\n"
                     "       \\? for help with gsql commands\n"
                     "       \\g or terminate with semicolon to execute query\n"
                     "       \\q to quit\n"));

            (void)fflush(stdout);
            continue;
        }

        /* echo back if flag is set */
        if (pset.echo == PSQL_ECHO_ALL && !pset.cur_cmd_interactive)
            puts(line);
        (void)fflush(stdout);
        /* insert newlines into query buffer between source lines */
        if (query_buf->len > 0) {
            appendPQExpBufferChar(query_buf, '\n');
            added_nl_pos = query_buf->len;
        } else
            added_nl_pos = -1; /* flag we didn't add one */

        /* Setting this will not have effect until next line. */
        die_on_error = pset.on_error_stop;
        /* Add processing of sql mode and terminator */
        bool is_new_lines = query_buf->len == 0 ? true : false;
        JudgeEndStateInBFormat(line, is_b_format, delimiter_name, is_new_lines);
        /*
         * Parse line, looking for command separators.
         */
        if (is_b_format) {
            line_temp = get_correct_str(line, delimiter_name, is_new_lines);
            psql_scan_setup(scan_state, line_temp, (int)strlen(line_temp));
            free(line_temp);
        } else {
            psql_scan_setup(scan_state, line, (int)strlen(line));
        }
        success = true;
        line_saved_in_history = false;

        while (success || !die_on_error) {
            PsqlScanResult scan_result;
            promptStatus_t prompt_tmp = prompt_status;

            scan_result = psql_scan(scan_state, query_buf, &prompt_tmp,is_b_format,delimiter_name);
            prompt_status = prompt_tmp;

            if (PQExpBufferBroken(query_buf)) {
                psql_error("out of memory\n");
                exit(EXIT_FAILURE);
            }

            /*
             * Send command if semicolon found, or if end of line and we're in
             * single-line mode.
             */
            if (scan_result == PSCAN_SEMICOLON || (scan_result == PSCAN_EOL && pset.singleline)) {
                                                
                /*
                 * Save query in history.  We use history_buf to accumulate
                 * multi-line queries into a single history entry.
                 */
                if (pset.cur_cmd_interactive && !line_saved_in_history) {
                    pg_append_history(line, history_buf);
                    pg_send_history(history_buf);
                    line_saved_in_history = true;
                }
                /* execute query */
                if (!pset.parallel && !query_count) {
#if defined(USE_ASSERT_CHECKING) || defined(FASTCHECK)
                    // Save parsed sql to sqlOutFile
                    if (pset.parseonly) {
                        if (isSlashEnd((const char*)line)) {
                            fprintf(pset.queryFout, "%s%s%s", query_buf->data, line, SPLITSTR);
                        } else {
                            fprintf(pset.queryFout, "%s%s", query_buf->data, SPLITSTR);
                        }
                    }
#endif
                    success = SendQuery(query_buf->data);

                    // Query fail, if need retry, invoke QueryRetryController().
                    //
                    if (!success && pset.retry_on) {
                        success = QueryRetryController(query_buf->data);
                    }
                } else {
                    size_t query_len = strlen(query_buf->data);

                    /* malloc memory for parallel query with MAX_STMTS once. */
                    if (query_count % MAX_STMTS == 0) {
                        if(NULL != query_stmts) {
                            char** temp = (char**)pg_calloc(1, sizeof(char*) * (query_count + MAX_STMTS));
                            rc = memcpy_s(temp, sizeof(char*) * query_count, query_stmts, sizeof(char*) * query_count);
                            securec_check_c(rc, "\0", "\0");

                            free(query_stmts);
                            query_stmts = temp;
                        }
                        else {
                            query_stmts = (char**)pg_calloc(1, sizeof(char*) * (query_count + MAX_STMTS));
                        }
                    }

                    query_stmts[query_count] = (char*)pg_malloc(sizeof(char) * (strlen(query_buf->data) + 1));
                    rc = strncpy_s(query_stmts[query_count], query_len + 1, query_buf->data, query_len);
                    securec_check_c(rc, "\0", "\0");
                    query_count++;
                }

                slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;

                /* transfer query to previous_buf by pointer-swapping */
                {
                    PQExpBuffer swap_buf = previous_buf;

                    previous_buf = query_buf;
                    query_buf = swap_buf;
                }
                resetPQExpBuffer(query_buf);
                /* Clear password related memory to avoid leaks when core. */
                if (pset.cur_cmd_interactive) {
                    size_t temp_len = strlen(query_buf->data);
                    rc = memset_s(query_buf->data, temp_len, 0, temp_len);
                    securec_check_c(rc, "\0", "\0");
                }
                added_nl_pos = -1;
                /* we need not do psql_scan_reset() here */
            } else if (scan_result == PSCAN_BACKSLASH) {
                /* handle backslash command */

                /*
                 * If we added a newline to query_buf, and nothing else has
                 * been inserted in query_buf by the lexer, then strip off the
                 * newline again.  This avoids any change to query_buf when a
                 * line contains only a backslash command.	Also, in this
                 * situation we force out any previous lines as a separate
                 * history entry; we don't want SQL and backslash commands
                 * intermixed in history if at all possible.
                 */
                if (query_buf->len == (unsigned int)(added_nl_pos)) {
                    query_buf->data[--query_buf->len] = '\0';
                    pg_send_history(history_buf);
                }
                added_nl_pos = -1;
#if defined(USE_ASSERT_CHECKING) || defined(FASTCHECK)
                if (pset.parseonly) {
                    fprintf(pset.queryFout, "%s%s", line, SPLITSTR);
                    scan_result = PSCAN_EOL;
                    isonlyparse = true;
                }
#endif
                if (!isonlyparse) {
                    slashCmdStatus = HandleSlashCmds(scan_state, query_buf->len > 0 ? query_buf : previous_buf);
                }

                /* execute parallel statements. */
                if (!pset.parallel && query_count) {
                    int i;
                    unsigned short int pager_saved = pset.popt.topt.pager;

                    /*
                     * For long queries in parallel, the main session of gsql may timeout,
                     * so we get the sessio_timeout first, and then set it to zero(no timeout).
                     * And reset it to the old value after the execution of all parallel queries.
                     */
                    char session_timeout_oldval[64] = "";

                    /* close pager in parallel execute. */
                    if (pager_saved) {
                        pset.popt.topt.pager = 0;
                    }

                    /* Get current value of session_timeout. */
                    GetSessionTimeout(session_timeout_oldval, sizeof(session_timeout_oldval));

                    /* We need to set the session_timeout parameter ? */
                    SetSessionTimeout("0");
                    success = do_parallel_execution(query_count, query_stmts);

                    /* Reset the session_timeout parameter after parallel queries. */
                    SetSessionTimeout(session_timeout_oldval);

                    for (i = 0; i < query_count; i++) {
                        if (query_stmts[i] != NULL) {
                            size_t temp_len = (size_t)strlen(query_stmts[i]);
                            rc = memset_s(query_stmts[i], temp_len, 0, temp_len);
                            securec_check_c(rc, "\0", "\0");

                            free(query_stmts[i]);
                            query_stmts[i] = NULL;
                        }
                    }
                    free(query_stmts);
                    query_stmts = NULL;
                    query_count = 0;

                    /* recovery the pager after parallel execute. */
                    pset.popt.topt.pager = pager_saved;

                    /* if set on_error_stop, -f parallel will exit directly. */
                    if (!pset.cur_cmd_interactive) {
                        if (!success && die_on_error) {
                            successResult = EXIT_USER;
                        }
                    }
                }

                /* save backslash command in history */
                if (pset.cur_cmd_interactive && !line_saved_in_history) {
                    pg_append_history(line, history_buf);
                    pg_send_history(history_buf);
                    line_saved_in_history = true;
                }

                success = slashCmdStatus != PSQL_CMD_ERROR;

                if ((slashCmdStatus == PSQL_CMD_SEND || slashCmdStatus == PSQL_CMD_NEWEDIT) && query_buf->len == 0) {
                    /* copy previous buffer to current for handling */
                    appendPQExpBufferStr(query_buf, previous_buf->data);
                }

                if (slashCmdStatus == PSQL_CMD_SEND) {
                    success = SendQuery(query_buf->data);

                    // Query fail, if need retry, invoke QueryRetryController().
                    //
                    if (!success && pset.retry_on) {
                        success = QueryRetryController(query_buf->data);
                    }

                    /* transfer query to previous_buf by pointer-swapping */
                    {
                        PQExpBuffer swap_buf = previous_buf;

                        previous_buf = query_buf;
                        query_buf = swap_buf;
                    }
                    resetPQExpBuffer(query_buf);

                    /* flush any paren nesting info after forced send */
                    psql_scan_reset(scan_state);
                } else if (slashCmdStatus == PSQL_CMD_NEWEDIT) {
                    /* rescan query_buf as new input */
                    psql_scan_finish(scan_state);
                    free(line);
                    line = pg_strdup(query_buf->data);
                    resetPQExpBuffer(query_buf);
                    /* reset parsing state since we are rescanning whole line */
                    psql_scan_reset(scan_state);
                    psql_scan_setup(scan_state, line, (int)strlen(line));
                    line_saved_in_history = false;
                    prompt_status = PROMPT_READY;
                } else if (slashCmdStatus == PSQL_CMD_TERMINATE) {
                    break;
                }
            }

            /* fall out of loop if lexer reached EOL */
            if (scan_result == PSCAN_INCOMPLETE || scan_result == PSCAN_EOL) {
                break;
            }
        }
        /* Add line to pending history if we didn't execute anything yet */
        if (pset.cur_cmd_interactive && !line_saved_in_history) {
            pg_append_history(line, history_buf);
        }

        psql_scan_finish(scan_state);

        /* Clear password related memory to avoid leaks when core. */
        if (pset.cur_cmd_interactive) {
            rc = memset_s(line, strlen(line), 0, strlen(line));
            securec_check_c(rc, "\0", "\0");
        }

        if (source_flag) {
            free(line);
            line = NULL;
        }

        if (slashCmdStatus == PSQL_CMD_TERMINATE) {
            successResult = EXIT_SUCCESS;
            break;
        }

        if (!pset.cur_cmd_interactive) {
            if (!success && die_on_error) {
                successResult = EXIT_USER;
            }
            /* Have we lost the db connection? */
            else if (pset.db == NULL) {
                successResult = EXIT_BADCONN;
            }
        }
    } /* while !endoffile/session */

    /*
     * Process query at the end of file without a semicolon
     */
    if (query_buf->len > 0 && !pset.cur_cmd_interactive && successResult == EXIT_SUCCESS) {
        /* save query in history */
        if (pset.cur_cmd_interactive) {
            pg_send_history(history_buf);
        }

        /* execute query */
        success = SendQuery(query_buf->data);

        // Query fail, if need retry, invoke QueryRetryController().
        //
        if (!success && pset.retry_on) {
            success = QueryRetryController(query_buf->data);
        }

        if (!success && die_on_error) {
            successResult = EXIT_USER;
        } else if (pset.db == NULL) {
            successResult = EXIT_BADCONN;
        } else
            successResult = success ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /*
     * Let's just make real sure the SIGINT handler won't try to use
     * sigint_interrupt_jmp after we exit this routine.  If there is an outer
     * MainLoop instance, it will reset sigint_interrupt_jmp to point to
     * itself at the top of its loop, before any further interactive input
     * happens.
     */
    sigint_interrupt_enabled = false;

    destroyPQExpBuffer(query_buf);
    destroyPQExpBuffer(previous_buf);
    destroyPQExpBuffer(history_buf);

    psql_scan_destroy(scan_state);

    pset.cur_cmd_source = prev_cmd_source;
    pset.cur_cmd_interactive = prev_cmd_interactive;
    pset.lineno = prev_lineno;

    return successResult;
} /* MainLoop() */

/*
 * psqlscan.c is #include'd here instead of being compiled on its own.
 * This is because we need postgres_fe.h to be read before any system
 * include files, else things tend to break on platforms that have
 * multiple infrastructures for stdio.h and so on.	flex is absolutely
 * uncooperative about that, so we can't compile psqlscan.c on its own.
 */
#include "psqlscan.inc"


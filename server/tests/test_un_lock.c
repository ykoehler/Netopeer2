/**
 * @file test_un_lock.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmocka np2srv <lock> and <unlock> test.
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <errno.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"

#define main server_main
#include "../config.h"
#undef NP2SRV_PIDFILE
#define NP2SRV_PIDFILE "/tmp/test_np2srv.pid"

#include "../main.c"

#undef main

volatile int initialized;
int pipes[2][2], p_in, p_out;

/*
 * SYSREPO WRAPPER FUNCTIONS
 */
int
__wrap_sr_connect(const char *app_name, const sr_conn_options_t opts, sr_conn_ctx_t **conn_ctx)
{
    (void)app_name;
    (void)opts;
    (void)conn_ctx;
    return SR_ERR_OK;
}

int
__wrap_sr_session_start(sr_conn_ctx_t *conn_ctx, const sr_datastore_t datastore,
                        const sr_sess_options_t opts, sr_session_ctx_t **session)
{
    (void)conn_ctx;
    (void)datastore;
    (void)opts;
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_list_schemas(sr_session_ctx_t *session, sr_schema_t **schemas, size_t *schema_cnt)
{
    (void)session;

    *schemas = calloc(1, sizeof **schemas);
    *schema_cnt = 1;
    (*schemas)[0].module_name = strdup("ietf-netconf-server");
    (*schemas)[0].installed = 1;

    return SR_ERR_OK;
}

int
__wrap_sr_get_schema(sr_session_ctx_t *session, const char *module_name, const char *revision,
                     const char *submodule_name, sr_schema_format_t format, char **schema_content)
{
    (void)session;
    (void)revision;
    (void)submodule_name;
    (void)format;

    if (!strcmp(module_name, "ietf-netconf-server")) {
        *schema_content = strdup("<module name=\"ietf-netconf-server\" xmlns=\"urn:ietf:params:xml:ns:yang:yin:1\"><namespace uri=\"ns\"/><prefix value=\"pr\"/></module>");
    }

    return SR_ERR_OK;
}

int
__wrap_sr_session_start_user(sr_conn_ctx_t *conn_ctx, const char *user_name, const sr_datastore_t datastore,
                             const sr_sess_options_t opts, sr_session_ctx_t **session)
{
    (void)conn_ctx;
    (void)user_name;
    (void)datastore;
    (void)opts;
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_session_stop(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

void
__wrap_sr_disconnect(sr_conn_ctx_t *conn_ctx)
{
    (void)conn_ctx;
}

int
__wrap_sr_session_refresh(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_module_install_subscribe(sr_session_ctx_t *session, sr_module_install_cb callback, void *private_ctx,
                                   sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)callback;
    (void)private_ctx;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_feature_enable_subscribe(sr_session_ctx_t *session, sr_feature_enable_cb callback, void *private_ctx,
                                   sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)callback;
    (void)private_ctx;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_module_change_subscribe(sr_session_ctx_t *session, const char *module_name, sr_module_change_cb callback,
                                  void *private_ctx, uint32_t priority, sr_subscr_options_t opts,
                                  sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)module_name;
    (void)callback;
    (void)private_ctx;
    (void)priority;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_get_items(sr_session_ctx_t *session, const char *xpath, sr_val_t **values, size_t *value_cnt)
{
    (void)session;
    (void)xpath;
    *values = NULL;
    *value_cnt = 0;
    return SR_ERR_OK;
}

#define LOCK_COUNT 16

int locks[3];
sr_datastore_t cur_ds;

int
__wrap_sr_session_switch_ds(sr_session_ctx_t *session, sr_datastore_t ds)
{
    (void)session;
    cur_ds = ds;
    return SR_ERR_OK;
}

int
__wrap_sr_discard_changes(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_lock_datastore(sr_session_ctx_t *session)
{
    (void)session;

    if (locks[cur_ds]) {
        return SR_ERR_LOCKED;
    }

    locks[cur_ds] = 1;
    return SR_ERR_OK;
}

int
__wrap_sr_unlock_datastore(sr_session_ctx_t *session)
{
    (void)session;

    if (!locks[cur_ds]) {
        return SR_ERR_OPERATION_FAILED;
    }

    locks[cur_ds] = 0;
    return SR_ERR_OK;
}

int
__wrap_sr_event_notif_send(sr_session_ctx_t *session, const char *xpath, const sr_val_t *values,
                           const size_t values_cnt, sr_ev_notif_flag_t opts)
{
    (void)session;
    (void)xpath;
    (void)values;
    (void)values_cnt;
    (void)opts;
    return SR_ERR_OK;
}

/*
 * LIBNETCONF2 WRAPPER FUNCTIONS
 */
struct nc_session {
    NC_STATUS status;
    NC_SESSION_TERM_REASON term_reason;
    int side;

    uint32_t id;
    int version;

    NC_TRANSPORT_IMPL ti_type;
    pthread_mutex_t *ti_lock;
    pthread_cond_t *ti_cond;
    volatile int *ti_inuse;
    union {
        struct {
            int in;
            int out;
        } fd;
#ifdef NC_ENABLED_SSH
        struct {
            void *channel;
            void *session;
            struct nc_session *next;
        } libssh;
#endif
#ifdef NC_ENABLED_TLS
        void *tls;
#endif
    } ti;
    const char *username;
    const char *host;
    uint16_t port;

    struct ly_ctx *ctx;
    void *data;
    uint8_t flags;

    union {
        struct {
            uint64_t msgid;
            const char **cpblts;
            struct nc_msg_cont *replies;
            struct nc_msg_cont *notifs;
            volatile pthread_t *ntf_tid;
        } client;
        struct {
            time_t session_start;
            time_t last_rpc;
            int ntf_status;
            pthread_mutex_t *ch_lock;
            pthread_cond_t *ch_cond;
#ifdef NC_ENABLED_SSH
            uint16_t ssh_auth_attempts;
#endif
#ifdef NC_ENABLED_TLS
            void *client_cert;
#endif
        } server;
    } opts;
};

NC_MSG_TYPE
__wrap_nc_accept(int timeout, struct nc_session **session)
{
    NC_MSG_TYPE ret;

    if (!initialized) {
        pipe(pipes[0]);
        pipe(pipes[1]);

        fcntl(pipes[0][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[0][1], F_SETFL, O_NONBLOCK);
        fcntl(pipes[1][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[1][1], F_SETFL, O_NONBLOCK);

        p_in = pipes[0][0];
        p_out = pipes[1][1];

        *session = calloc(1, sizeof **session);
        (*session)->status = NC_STATUS_RUNNING;
        (*session)->side = 1;
        (*session)->id = 1;
        (*session)->ti_lock = malloc(sizeof *(*session)->ti_lock);
        pthread_mutex_init((*session)->ti_lock, NULL);
        (*session)->ti_cond = malloc(sizeof *(*session)->ti_cond);
        pthread_cond_init((*session)->ti_cond, NULL);
        (*session)->ti_inuse = malloc(sizeof *(*session)->ti_inuse);
        *(*session)->ti_inuse = 0;
        (*session)->ti_type = NC_TI_FD;
        (*session)->ti.fd.in = pipes[1][0];
        (*session)->ti.fd.out = pipes[0][1];
        (*session)->ctx = np2srv.ly_ctx;
        (*session)->flags = 1; //shared ctx
        (*session)->username = "user1";
        (*session)->host = "localhost";
        (*session)->opts.server.session_start = (*session)->opts.server.last_rpc = time(NULL);
        printf("test: New session 1\n");
        initialized = 1;
        ret = NC_MSG_HELLO;
    } else {
        usleep(timeout * 1000);
        ret = NC_MSG_WOULDBLOCK;
    }

    return ret;
}

void
__wrap_nc_session_free(struct nc_session *session, void (*data_free)(void *))
{
    if (data_free) {
        data_free(session->data);
    }
    pthread_mutex_destroy(session->ti_lock);
    free(session->ti_lock);
    pthread_cond_destroy(session->ti_cond);
    free(session->ti_cond);
    free((int *)session->ti_inuse);
    free(session);
}

int
__wrap_nc_server_endpt_count(void)
{
    return 1;
}

/*
 * SERVER THREAD
 */
pthread_t server_tid;
static void *
server_thread(void *arg)
{
    (void)arg;
    char *argv[] = {"netopeer2-server", "-d", "-v2"};

    return (void *)(int64_t)server_main(3, argv);
}

/*
 * TEST
 */
static void
test_write(int fd, const char *data, int line)
{
    int ret, written, to_write;

    written = 0;
    to_write = strlen(data);
    do {
        ret = write(fd, data + written, to_write - written);
        if (ret == -1) {
            if (errno != EAGAIN) {
                fprintf(stderr, "write fail (%s, line %d)\n", strerror(errno), line);
                fail();
            }
            usleep(100000);
            ret = 0;
        }
        written += ret;
    } while (written < to_write);

    while (((ret = write(fd, "]]>]]>", 6)) == -1) && (errno == EAGAIN));
    if (ret == -1) {
        fprintf(stderr, "write fail (%s, line %d)\n", strerror(errno), line);
        fail();
    } else if (ret < 6) {
        fprintf(stderr, "write fail (end tag, written only %d bytes, line %d)\n", ret, line);
        fail();
    }
}

static void
test_read(int fd, const char *template, int line)
{
    char *buf, *ptr;
    int ret, red, to_read;

    red = 0;
    to_read = strlen(template);
    buf = malloc(to_read + 1);
    do {
        ret = read(fd, buf + red, to_read - red);
        if (ret == -1) {
            if (errno != EAGAIN) {
                fprintf(stderr, "read fail (%s, line %d)\n", strerror(errno), line);
                fail();
            }
            usleep(100000);
            ret = 0;
        }
        red += ret;

        /* premature ending tag check */
        if ((red > 5) && !strncmp((buf + red) - 6, "]]>]]>", 6)) {
            break;
        }
    } while (red < to_read);
    buf[red] = '\0';

    /* unify all datetimes */
    for (ptr = strstr(buf, "+02:00"); ptr; ptr = strstr(ptr + 1, "+02:00")) {
        if ((ptr[-3] == ':') && (ptr[-6] == ':') && (ptr[-9] == 'T') && (ptr[-12] == '-') && (ptr[-15] == '-')) {
            strncpy(ptr - 19, "0000-00-00T00:00:00", 19);
        }
    }

    for (red = 0; buf[red]; ++red) {
        if (buf[red] != template[red]) {
            fprintf(stderr, "read fail (non-matching template, line %d)\n\"%s\"(%d)\nvs. template\n\"%s\"\n",
                    line, buf + red, red, template + red);
            fail();
        }
    }

    /* read ending tag */
    while (((ret = read(fd, buf, 6)) == -1) && (errno == EAGAIN));
    if (ret == -1) {
        fprintf(stderr, "read fail (%s, line %d)\n", strerror(errno), line);
        fail();
    }
    buf[ret] = '\0';
    if ((ret < 6) || strcmp(buf, "]]>]]>")) {
        fprintf(stderr, "read fail (end tag \"%s\", line %d)\n", buf, line);
        fail();
    }

    free(buf);
}

static int
np_start(void **state)
{
    (void)state; /* unused */

    control = LOOP_CONTINUE;
    initialized = 0;
    assert_int_equal(pthread_create(&server_tid, NULL, server_thread, NULL), 0);

    while (!initialized) {
        usleep(100000);
    }

    return 0;
}

static int
np_stop(void **state)
{
    (void)state; /* unused */
    int64_t ret;

    control = LOOP_STOP;
    assert_int_equal(pthread_join(server_tid, (void **)&ret), 0);

    close(pipes[0][0]);
    close(pipes[0][1]);
    close(pipes[1][0]);
    close(pipes[1][1]);
    return ret;
}

static void
test_lock1(void **state)
{
    (void)state; /* unused */
    const char *lock_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<lock>"
            "<target>"
                "<startup/>"
            "</target>"
        "</lock>"
    "</rpc>";
    const char *lock_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<ok/>"
    "</rpc-reply>";

    test_write(p_out, lock_rpc, __LINE__);
    test_read(p_in, lock_rpl, __LINE__);
}

static void
test_lock2(void **state)
{
    (void)state; /* unused */
    const char *lock_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<lock>"
            "<target>"
                "<startup/>"
            "</target>"
        "</lock>"
    "</rpc>";
    const char *lock_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<rpc-error>"
            "<error-type>protocol</error-type>"
            "<error-tag>lock-denied</error-tag>"
            "<error-severity>error</error-severity>"
            "<error-message xml:lang=\"en\">Locking datastore startup by session 1 failed (datastore is already locked by session 1).</error-message>"
            "<error-info>"
                "<session-id>1</session-id>"
            "</error-info>"
        "</rpc-error>"
    "</rpc-reply>";

    test_write(p_out, lock_rpc, __LINE__);
    test_read(p_in, lock_rpl, __LINE__);
}

static void
test_unlock1(void **state)
{
    (void)state; /* unused */
    const char *lock_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<unlock>"
            "<target>"
                "<startup/>"
            "</target>"
        "</unlock>"
    "</rpc>";
    const char *lock_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<ok/>"
    "</rpc-reply>";

    test_write(p_out, lock_rpc, __LINE__);
    test_read(p_in, lock_rpl, __LINE__);
}

static void
test_unlock2(void **state)
{
    (void)state; /* unused */
    const char *lock_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<unlock>"
            "<target>"
                "<startup/>"
            "</target>"
        "</unlock>"
    "</rpc>";
    const char *lock_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<rpc-error>"
            "<error-type>protocol</error-type>"
            "<error-tag>operation-failed</error-tag>"
            "<error-severity>error</error-severity>"
            "<error-message xml:lang=\"en\">Unlocking datastore startup by session 1 failed (lock is not active).</error-message>"
        "</rpc-error>"
    "</rpc-reply>";

    test_write(p_out, lock_rpc, __LINE__);
    test_read(p_in, lock_rpl, __LINE__);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup(test_lock1, np_start),
                    cmocka_unit_test(test_lock2),
                    cmocka_unit_test(test_unlock1),
                    cmocka_unit_test_teardown(test_unlock2, np_stop),
    };

    if (setenv("CMOCKA_TEST_ABORT", "1", 1)) {
        fprintf(stderr, "Cannot set Cmocka thread environment variable.\n");
    }
    return cmocka_run_group_tests(tests, NULL, NULL);
}

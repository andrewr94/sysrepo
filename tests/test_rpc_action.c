/**
 * @file test_rpc_action.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief test for sending/receiving RPCs/actions
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE

#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <stdarg.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "tests/config.h"
#include "sysrepo.h"

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
    volatile int cb_called;
    pthread_barrier_t barrier;
};

static int
setup(void **state)
{
    struct state *st;
    uint32_t conn_count;

    st = calloc(1, sizeof *st);
    *state = st;

    sr_connection_count(&conn_count);
    assert_int_equal(conn_count, 0);

    st->cb_called = 0;
    pthread_barrier_init(&st->barrier, NULL, 2);

    if (sr_connect(0, &(st->conn)) != SR_ERR_OK) {
        return 1;
    }

    if (sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/iana-if-type.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/ops-ref.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/ops.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    sr_disconnect(st->conn);

    if (sr_connect(0, &(st->conn)) != SR_ERR_OK) {
        return 1;
    }

    if (sr_session_start(st->conn, SR_DS_RUNNING, &st->sess) != SR_ERR_OK) {
        return 1;
    }

    sr_session_set_nc_id(st->sess, 128);

    return 0;
}

static int
teardown(void **state)
{
    struct state *st = (struct state *)*state;
    int ret = 0;

    ret += sr_remove_module(st->conn, "ops");
    ret += sr_remove_module(st->conn, "ops-ref");
    ret += sr_remove_module(st->conn, "iana-if-type");
    ret += sr_remove_module(st->conn, "ietf-interfaces");
    ret += sr_remove_module(st->conn, "test");

    sr_disconnect(st->conn);
    pthread_barrier_destroy(&st->barrier);
    free(st);
    return ret;
}

static int
clear_ops(void **state)
{
    struct state *st = (struct state *)*state;

    sr_delete_item(st->sess, "/ops-ref:l1", 0);
    sr_delete_item(st->sess, "/ops-ref:l2", 0);
    sr_delete_item(st->sess, "/ops:cont", 0);
    sr_apply_changes(st->sess);

    return 0;
}

/* TEST 1 */
static int
rpc_fail_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    char *str1;
    const char *str2;
    int ret;

    (void)event;
    (void)request_id;
    (void)output;
    (void)private_data;

    assert_int_equal(sr_session_get_nc_id(session), 128);
    assert_string_equal(xpath, "/ops:rpc1");

    /* check input data tree */
    ret = lyd_print_mem(&str1, input, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    str2 = "<rpc1 xmlns=\"urn:ops\"></rpc1>";

    assert_string_equal(str1, str2);
    free(str1);

    /* error */
    sr_set_error(session, "RPC FAIL", NULL);
    return SR_ERR_SYS;
}

static void
test_fail(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    const sr_error_info_t *err_info = NULL;
    struct lyd_node *input, *output;
    int ret;

    /* subscribe /*/
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:rpc1", rpc_fail_cb, NULL, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* send RPC */
    input = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:rpc1", NULL, 0, 0);
    assert_non_null(input);

    /* expect an error */
    ret = sr_rpc_send_tree(st->sess, input, &output);
    lyd_free_withsiblings(input);
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);
    ret = sr_get_error(st->sess, &err_info);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(err_info->err_count, 1);
    assert_string_equal(err_info->err[0].message, "RPC FAIL");
    assert_null(err_info->err[0].xpath);
    assert_null(output);

    /* try to send an action */
    input = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='1']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input);

    ret = sr_rpc_send_tree(st->sess, input, &output);
    for (; input->parent; input = input->parent);
    lyd_free_withsiblings(input);
    assert_int_equal(ret, SR_ERR_INVAL_ARG);

    sr_unsubscribe(subscr);
}

/* TEST 2 */
static int
rpc_rpc_cb(sr_session_ctx_t *session, const char *xpath, const sr_val_t *input, const size_t input_cnt, sr_event_t event,
        uint32_t request_id, sr_val_t **output, size_t *output_cnt, void *private_data)
{
    static int rpc2_called = 0;

    (void)session;
    (void)event;
    (void)request_id;
    (void)private_data;

    if (!strcmp(xpath, "/ops:rpc1")) {
        /* check input data */
        assert_int_equal(input_cnt, 2);
        assert_string_equal(input[0].xpath, "/ops:rpc1/l1");
        assert_string_equal(input[1].xpath, "/ops:rpc1/l2");
        assert_int_equal(input[1].dflt, 1);

        /* create (empty) output data */
    } else if (!strcmp(xpath, "/ops:rpc2")) {
        /* check (empty) input data tree */
        assert_int_equal(input_cnt, 0);

        if (rpc2_called == 0) {
            /* create (invalid) output data */
            *output_cnt = 1;
            *output = calloc(*output_cnt, sizeof **output);

            (*output)[0].xpath = strdup("/ops:rpc2/cont/l3");
            (*output)[0].type = SR_STRING_T;
            (*output)[0].data.string_val = strdup("inval-ref");
        } else if (rpc2_called == 1) {
            /* create output data */
            *output_cnt = 1;
            *output = calloc(*output_cnt, sizeof **output);

            (*output)[0].xpath = strdup("/ops:rpc2/cont/l3");
            (*output)[0].type = SR_STRING_T;
            (*output)[0].data.string_val = strdup("l2-val");
        } else {
            fail();
        }
        ++rpc2_called;
    } else if (!strcmp(xpath, "/ops:rpc3")) {
        /* check input data */
        assert_int_equal(input_cnt, 1);
        assert_string_equal(input[0].xpath, "/ops:rpc3/l4");

        /* create output data */
        *output_cnt = 1;
        *output = calloc(*output_cnt, sizeof **output);

        (*output)[0].xpath = strdup("/ops:rpc3/l5");
        (*output)[0].type = SR_UINT16_T;
        (*output)[0].data.uint16_val = 256;
    } else {
        fail();
    }

    return SR_ERR_OK;
}

static int
module_change_dummy_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    (void)session;
    (void)module_name;
    (void)xpath;
    (void)event;
    (void)request_id;
    (void)private_data;

    return SR_ERR_OK;
}

static void
test_rpc(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    const sr_error_info_t *err_info = NULL;
    sr_val_t input, *output;
    size_t output_count;
    int ret;

    /* subscribe */
    ret = sr_rpc_subscribe(st->sess, "/ops:rpc1", rpc_rpc_cb, NULL, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe(st->sess, "/ops:rpc2", rpc_rpc_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe(st->sess, "/ops:rpc3", rpc_rpc_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* set some data needed for validation */
    ret = sr_set_item_str(st->sess, "/ops-ref:l1", "l1-val", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops-ref:l2", "l2-val", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create first RPC
     */
    input.xpath = "/ops:rpc1/l1";
    input.type = SR_STRING_T;
    input.data.string_val = "l1-val";
    input.dflt = 0;

    /* try to send first RPC, expect an error */
    ret = sr_rpc_send(st->sess, "/ops:rpc1", &input, 1, &output, &output_count);
    assert_int_equal(ret, SR_ERR_VALIDATION_FAILED);
    ret = sr_get_error(st->sess, &err_info);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(err_info->err_count, 2);
    assert_string_equal(err_info->err[0].message, "Leafref \"/ops-ref:l1\" of value \"l1-val\" points to a non-existing leaf.");
    assert_string_equal(err_info->err[0].xpath, "/ops:rpc1/l1");
    assert_string_equal(err_info->err[1].message, "RPC input validation failed.");
    assert_null(err_info->err[1].xpath);
    assert_null(output);
    assert_int_equal(output_count, 0);

    /* subscribe to the data so they are actually present in operational */
    ret = sr_module_change_subscribe(st->sess, "ops-ref", NULL, module_change_dummy_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* try to send first RPC again, now should succeed */
    ret = sr_rpc_send(st->sess, "/ops:rpc1", &input, 1, &output, &output_count);
    assert_int_equal(ret, SR_ERR_OK);

    /* check output data tree */
    assert_null(output);
    assert_int_equal(output_count, 0);

    /*
     * create second RPC (no input)
     */

    /* try to send second RPC, expect an error */
    ret = sr_rpc_send(st->sess, "/ops:rpc2", NULL, 0, &output, &output_count);
    assert_int_equal(ret, SR_ERR_VALIDATION_FAILED);
    ret = sr_get_error(st->sess, &err_info);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(err_info->err_count, 2);
    assert_string_equal(err_info->err[0].message, "Leafref \"/ops-ref:l2\" of value \"inval-ref\" points to a non-existing leaf.");
    assert_string_equal(err_info->err[0].xpath, "/ops:rpc2/cont/l3");
    assert_string_equal(err_info->err[1].message, "RPC output validation failed.");
    assert_null(err_info->err[1].xpath);

    /* try to send second RPC again, should succeed now */
    ret = sr_rpc_send(st->sess, "/ops:rpc2", NULL, 0, &output, &output_count);
    assert_int_equal(ret, SR_ERR_OK);

    /* check output data tree */
    assert_non_null(output);
    assert_int_equal(output_count, 2);

    assert_string_equal(output[0].xpath, "/ops:rpc2/cont");
    assert_string_equal(output[1].xpath, "/ops:rpc2/cont/l3");
    assert_string_equal(output[1].data.string_val, "l2-val");

    sr_free_values(output, output_count);

    /*
     * create third RPC
     */
    input.xpath = "/ops:rpc3/l4";
    input.type = SR_STRING_T;
    input.data.string_val = "some-val";
    input.dflt = 0;

    /* send third RPC */
    ret = sr_rpc_send(st->sess, "/ops:rpc3", &input, 1, &output, &output_count);
    assert_int_equal(ret, SR_ERR_OK);

    /* check output data tree */
    assert_non_null(output);
    assert_int_equal(output_count, 1);

    assert_string_equal(output[0].xpath, "/ops:rpc3/l5");
    assert_int_equal(output[0].data.uint16_val, 256);

    sr_free_values(output, output_count);

    sr_unsubscribe(subscr);
}

/* TEST 3 */
static int
rpc_action_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    struct lyd_node *node;
    char *str1;
    const char *str2;
    int ret;

    (void)session;
    (void)event;
    (void)request_id;
    (void)private_data;

    if (!strcmp(xpath, "/ops:cont/list1/cont2/act1")) {
        /* check input data */
        ret = lyd_print_mem(&str1, input, LYD_XML, LYP_WITHSIBLINGS);
        assert_int_equal(ret, 0);
        str2 = "<act1 xmlns=\"urn:ops\"><l6>val</l6><l7>val</l7></act1>";
        assert_string_equal(str1, str2);
        free(str1);

        /* create output data */
        node = lyd_new_path(output, NULL, "l9", "l12-val", 0, LYD_PATH_OPT_OUTPUT);
        assert_non_null(node);
    } else if (!strcmp(xpath, "/ops:cont/list1/act2")) {
        /* check input data */
        ret = lyd_print_mem(&str1, input, LYD_XML, LYP_WITHSIBLINGS);
        assert_int_equal(ret, 0);
        str2 = "<act2 xmlns=\"urn:ops\"><l10>e3</l10></act2>";
        assert_string_equal(str1, str2);
        free(str1);

        /* create output data */
        node = lyd_new_path(output, NULL, "l11", "-65536", 0, LYD_PATH_OPT_OUTPUT);
        assert_non_null(node);
    } else {
        fail();
    }

    return SR_ERR_OK;
}

static void
test_action(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    struct lyd_node *node, *input_op, *output_op;
    char *str1;
    const char *str2;
    int ret;

    /* subscribe */
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1/cont2/act1", rpc_action_cb, NULL, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1/act2", rpc_action_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* set some data needed for validation and executing the actions */
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='key']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/l12", "l12-val", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* subscribe to the data so they are actually present in operational */
    ret = sr_module_change_subscribe(st->sess, "ops", NULL, module_change_dummy_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create first action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='key']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val", 0, 0);
    assert_non_null(node);

    /* send first action */
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    assert_int_equal(ret, SR_ERR_OK);

    /* check output data tree */
    assert_non_null(output_op);
    ret = lyd_print_mem(&str1, output_op, LYD_XML, LYP_WITHSIBLINGS);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);
    assert_int_equal(ret, 0);
    str2 = "<act1 xmlns=\"urn:ops\"><l9>l12-val</l9></act1>";
    assert_string_equal(str1, str2);
    free(str1);

    /*
     * create second action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='key']/act2", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l10", "e3", 0, 0);
    assert_non_null(node);

    /* send second action */
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    assert_int_equal(ret, SR_ERR_OK);

    /* check output data tree */
    assert_non_null(output_op);
    ret = lyd_print_mem(&str1, output_op, LYD_XML, LYP_WITHSIBLINGS);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);
    assert_int_equal(ret, 0);
    str2 = "<act2 xmlns=\"urn:ops\"><l11>-65536</l11></act2>";
    assert_string_equal(str1, str2);
    free(str1);

    sr_unsubscribe(subscr);
}

/* TEST 4 */
static int
rpc_action_pred_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    char *str1;
    const char *str2;
    int ret;

    (void)session;
    (void)event;
    (void)request_id;
    (void)output;
    (void)private_data;

    if (!strcmp(xpath, "/ops:cont/list1[k='one' or k='two']/cont2/act1")) {
        /* check input data */
        ret = lyd_print_mem(&str1, input, LYD_XML, LYP_WITHSIBLINGS);
        assert_int_equal(ret, 0);
        str2 = "<act1 xmlns=\"urn:ops\"><l6>val2</l6><l7>val2</l7></act1>";
        assert_string_equal(str1, str2);
        free(str1);
    } else if (!strcmp(xpath, "/ops:cont/list1[k='three' or k='four']/cont2/act1")) {
        /* check input data */
        ret = lyd_print_mem(&str1, input, LYD_XML, LYP_WITHSIBLINGS);
        assert_int_equal(ret, 0);
        str2 = "<act1 xmlns=\"urn:ops\"><l6>val3</l6><l7>val3</l7></act1>";
        assert_string_equal(str1, str2);
        free(str1);
    } else {
        fail();
    }

    return SR_ERR_OK;
}

static void
test_action_pred(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    struct lyd_node *node, *input_op, *output_op;
    int ret;

    /* you cannot subscribe to more actions */
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1/cont2/act1 or /ops:rpc1", rpc_action_pred_cb, NULL, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_LY);

    /* subscribe with some predicates */
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1[k='one' or k='two']/cont2/act1", rpc_action_pred_cb, NULL, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1[k='three' or k='four']/cont2/act1", rpc_action_pred_cb, NULL,
            0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* set some data needed for validation and executing the actions */
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='zero']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='one']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='two']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='three']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='key']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* subscribe to the data so they are actually present in operational */
    ret = sr_module_change_subscribe(st->sess, "ops", NULL, module_change_dummy_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create first action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='zero']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val", 0, 0);
    assert_non_null(node);

    /* send action, fails */
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    assert_int_equal(ret, SR_ERR_UNSUPPORTED);

    /*
     * create second action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='one']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val2", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val2", 0, 0);
    assert_non_null(node);

    /* send action, should be fine */
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);

    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create third action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='three']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val3", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val3", 0, 0);
    assert_non_null(node);

    /* send action, should be fine */
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);

    assert_int_equal(ret, SR_ERR_OK);

    sr_unsubscribe(subscr);
}

/* TEST 5 */
static int
rpc_multi_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    struct state *st = (struct state *)private_data;

    (void)session;
    (void)xpath;
    (void)input;
    (void)event;
    (void)request_id;
    (void)output;

    ++st->cb_called;
    return SR_ERR_OK;
}

static void
test_multi(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    struct lyd_node *node, *input_op, *output_op;
    int ret;

    /* subscribe */
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1/cont2/act1", rpc_multi_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1[k='one' or k='two']/cont2/act1", rpc_multi_cb, st,
            0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:cont/list1[k='two' or k='three' or k='four']/cont2/act1",
            rpc_multi_cb, st, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* set some data needed for validation and executing the actions */
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='zero']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='one']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='two']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='three']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(st->sess, "/ops:cont/list1[k='key']", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* subscribe to the data so they are actually present in operational */
    ret = sr_module_change_subscribe(st->sess, "ops", NULL, module_change_dummy_cb, NULL, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create first action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='zero']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val", 0, 0);
    assert_non_null(node);

    /* send action */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);

    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(st->cb_called, 1);

    /*
     * create second action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='one']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val2", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val2", 0, 0);
    assert_non_null(node);

    /* send action */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);

    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(st->cb_called, 2);

    /*
     * create third action
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:cont/list1[k='two']/cont2/act1", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l6", "val3", 0, 0);
    assert_non_null(node);
    node = lyd_new_path(input_op, NULL, "l7", "val3", 0, 0);
    assert_non_null(node);

    /* send action */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    for (; input_op->parent; input_op = input_op->parent);
    lyd_free_withsiblings(input_op);
    for (; output_op->parent; output_op = output_op->parent);
    lyd_free_withsiblings(output_op);

    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(st->cb_called, 3);

    sr_unsubscribe(subscr);
}

/* TEST 6 */
static int
rpc_multi_fail0_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    struct state *st = (struct state *)private_data;
    struct lyd_node *node;
    static int call_no = 1;
    int ret = SR_ERR_OK;

    (void)session;
    (void)xpath;
    (void)input;
    (void)request_id;

    ++st->cb_called;

    /* create output data in all cases, it should always be freed */
    node = lyd_new_path(output, NULL, "l5", "0", 0, LYD_PATH_OPT_OUTPUT);
    assert_non_null(node);

    switch (call_no) {
    case 1:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 3);
        /* callback fails */
        ret = SR_ERR_NOMEM;
        ++call_no;
        break;
    case 2:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 3);
        ++call_no;
        break;
    default:
        fail();
    }

    return ret;
}

static int
rpc_multi_fail1_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    struct state *st = (struct state *)private_data;
    struct lyd_node *node;
    static int call_no = 1;
    int ret = SR_ERR_OK;

    (void)session;
    (void)xpath;
    (void)input;
    (void)request_id;

    ++st->cb_called;

    /* create output data in all cases, it should always be freed */
    node = lyd_new_path(output, NULL, "l5", "1", 0, LYD_PATH_OPT_OUTPUT);
    assert_non_null(node);

    switch (call_no) {
    case 1:
        if (event == SR_EV_RPC) {
            assert_int_equal(st->cb_called, 2);
        } else {
            assert_int_equal(event, SR_EV_ABORT);
            assert_int_equal(st->cb_called, 5);
            ++call_no;
            /* last callback */
            pthread_barrier_wait(&st->barrier);
        }
        break;
    case 2:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 2);
        /* callback fails */
        ret = SR_ERR_NOT_FOUND;
        ++call_no;
        break;
    case 3:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 2);
        ++call_no;
        break;
    default:
        fail();
    }

    return ret;
}

static int
rpc_multi_fail2_cb(sr_session_ctx_t *session, const char *xpath, const struct lyd_node *input, sr_event_t event,
        uint32_t request_id, struct lyd_node *output, void *private_data)
{
    struct state *st = (struct state *)private_data;
    struct lyd_node *node;
    static int call_no = 1;
    int ret = SR_ERR_OK;

    (void)session;
    (void)xpath;
    (void)input;
    (void)request_id;

    ++st->cb_called;

    /* create output data in all cases, it should always be freed */
    node = lyd_new_path(output, NULL, "l5", "2", 0, LYD_PATH_OPT_OUTPUT);
    assert_non_null(node);

    switch (call_no) {
    case 1:
        if (event == SR_EV_RPC) {
            assert_int_equal(st->cb_called, 1);
        } else {
            assert_int_equal(event, SR_EV_ABORT);
            assert_int_equal(st->cb_called, 4);
            ++call_no;
        }
        break;
    case 2:
        if (event == SR_EV_RPC) {
            assert_int_equal(st->cb_called, 1);
        } else {
            assert_int_equal(event, SR_EV_ABORT);
            assert_int_equal(st->cb_called, 3);
            ++call_no;
            /* last callback */
            pthread_barrier_wait(&st->barrier);
        }
        break;
    case 3:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 1);
        /* callback fails, last callback (but there is no callback for abort, synchronizing would block) */
        ret = SR_ERR_BAD_ELEMENT;
        ++call_no;
        break;
    case 4:
        assert_int_equal(event, SR_EV_RPC);
        assert_int_equal(st->cb_called, 1);
        ++call_no;
        break;
    default:
        fail();
    }

    return ret;
}

static void
test_multi_fail(void **state)
{
    struct state *st = (struct state *)*state;
    sr_subscription_ctx_t *subscr;
    struct lyd_node *node, *input_op, *output_op;
    int ret;

    /* subscribe */
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:rpc3", rpc_multi_fail0_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:rpc3", rpc_multi_fail1_cb, st, 1, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_rpc_subscribe_tree(st->sess, "/ops:rpc3", rpc_multi_fail2_cb, st, 2, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /*
     * create first RPC
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:rpc3", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l4", "val", 0, 0);
    assert_non_null(node);

    /* send RPC */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    lyd_free_withsiblings(input_op);
    lyd_free_withsiblings(output_op);

    pthread_barrier_wait(&st->barrier);

    /* it should fail with 5 total callback calls */
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);
    assert_int_equal(st->cb_called, 5);

    /*
     * create second RPC
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:rpc3", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l4", "val", 0, 0);
    assert_non_null(node);

    /* send RPC */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    lyd_free_withsiblings(input_op);
    lyd_free_withsiblings(output_op);

    pthread_barrier_wait(&st->barrier);

    /* it should fail with 3 total callback calls */
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);
    assert_int_equal(st->cb_called, 3);

    /*
     * create third RPC
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:rpc3", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l4", "val", 0, 0);
    assert_non_null(node);

    /* send RPC */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    lyd_free_withsiblings(input_op);
    lyd_free_withsiblings(output_op);

    /* it should fail with 1 total callback calls */
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);
    assert_int_equal(st->cb_called, 1);

    /*
     * create fourth RPC
     */
    input_op = lyd_new_path(NULL, sr_get_context(st->conn), "/ops:rpc3", NULL, 0, LYD_PATH_OPT_NOPARENTRET);
    assert_non_null(input_op);
    node = lyd_new_path(input_op, NULL, "l4", "val", 0, 0);
    assert_non_null(node);

    /* send RPC */
    st->cb_called = 0;
    ret = sr_rpc_send_tree(st->sess, input_op, &output_op);
    lyd_free_withsiblings(input_op);

    /* it should not fail */
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(st->cb_called, 3);

    /* check output */
    assert_string_equal(output_op->child->schema->name, "l5");
    assert_int_equal(((struct lyd_node_leaf_list *)output_op->child)->value.uint16, 0);
    lyd_free_withsiblings(output_op);

    sr_unsubscribe(subscr);
}

/* MAIN */
int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fail),
        cmocka_unit_test_teardown(test_rpc, clear_ops),
        cmocka_unit_test_teardown(test_action, clear_ops),
        cmocka_unit_test_teardown(test_action_pred, clear_ops),
        cmocka_unit_test_teardown(test_multi, clear_ops),
        cmocka_unit_test(test_multi_fail),
    };

    sr_log_stderr(SR_LL_INF);
    return cmocka_run_group_tests(tests, setup, teardown);
}

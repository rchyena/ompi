/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"
#include "opal/constants.h"
#include "opal/types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/dss/dss.h"
#include "opal/mca/event/event.h"
#include "opal/mca/hwloc/base/base.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_progress_threads.h"
#include "opal/util/argv.h"
#include "opal/util/error.h"
#include "opal/util/output.h"
#include "opal/util/proc.h"
#include "opal/util/show_help.h"

#include "pmix_ext20.h"
#include "opal/mca/pmix/base/base.h"
#include "opal/mca/pmix/pmix_types.h"

#include <pmix/pmix_common.h>

/****    C.O.M.M.O.N   I.N.T.E.R.F.A.C.E.S     ****/

/* These are functions used by both client and server to
 * access common functions in the embedded PMIx library */

static const char *pmix20_get_nspace(opal_jobid_t jobid);
static void pmix20_register_jobid(opal_jobid_t jobid, const char *nspace);
static void register_handler(opal_list_t *event_codes,
                             opal_list_t *info,
                             opal_pmix_notification_fn_t evhandler,
                             opal_pmix_evhandler_reg_cbfunc_t cbfunc,
                             void *cbdata);
static void deregister_handler(size_t evhandler,
                               opal_pmix_op_cbfunc_t cbfunc,
                               void *cbdata);
static int notify_event(int status,
                        const opal_process_name_t *source,
                        opal_pmix_data_range_t range,
                        opal_list_t *info,
                        opal_pmix_op_cbfunc_t cbfunc, void *cbdata);

const opal_pmix_base_module_t opal_pmix_ext20_module = {
    /* client APIs */
    .init = pmix20_client_init,
    .finalize = pmix20_client_finalize,
    .initialized = pmix20_initialized,
    .abort = pmix20_abort,
    .commit = pmix20_commit,
    .fence = pmix20_fence,
    .fence_nb = pmix20_fencenb,
    .put = pmix20_put,
    .get = pmix20_get,
    .get_nb = pmix20_getnb,
    .publish = pmix20_publish,
    .publish_nb = pmix20_publishnb,
    .lookup = pmix20_lookup,
    .lookup_nb = pmix20_lookupnb,
    .unpublish = pmix20_unpublish,
    .unpublish_nb = pmix20_unpublishnb,
    .spawn = pmix20_spawn,
    .spawn_nb = pmix20_spawnnb,
    .connect = pmix20_connect,
    .connect_nb = pmix20_connectnb,
    .disconnect = pmix20_disconnect,
    .disconnect_nb = pmix20_disconnectnb,
    .resolve_peers = pmix20_resolve_peers,
    .resolve_nodes = pmix20_resolve_nodes,
    /* server APIs */
    .server_init = pmix20_server_init,
    .server_finalize = pmix20_server_finalize,
    .generate_regex = pmix20_server_gen_regex,
    .generate_ppn = pmix20_server_gen_ppn,
    .server_register_nspace = pmix20_server_register_nspace,
    .server_deregister_nspace = pmix20_server_deregister_nspace,
    .server_register_client = pmix20_server_register_client,
    .server_deregister_client = pmix20_server_deregister_client,
    .server_setup_fork = pmix20_server_setup_fork,
    .server_dmodex_request = pmix20_server_dmodex,
    .server_notify_event = pmix20_server_notify_event,
    /* utility APIs */
    .get_version = PMIx_Get_version,
    .register_evhandler = register_handler,
    .deregister_evhandler = deregister_handler,
    .notify_event = notify_event,
    .store_local = pmix20_store_local,
    .get_nspace = pmix20_get_nspace,
    .register_jobid = pmix20_register_jobid
};

static const char *pmix20_get_nspace(opal_jobid_t jobid)
{
    opal_pmix20_jobid_trkr_t *jptr;

    OPAL_LIST_FOREACH(jptr, &mca_pmix_ext20_component.jobids, opal_pmix20_jobid_trkr_t) {
        if (jptr->jobid == jobid) {
            return jptr->nspace;
        }
    }
    return NULL;
}

static void pmix20_register_jobid(opal_jobid_t jobid, const char *nspace)
{
    opal_pmix20_jobid_trkr_t *jptr;

    /* if we don't already have it, add this to our jobid tracker */
    OPAL_LIST_FOREACH(jptr, &mca_pmix_ext20_component.jobids, opal_pmix20_jobid_trkr_t) {
        if (jptr->jobid == jobid) {
            return;
        }
    }
    jptr = OBJ_NEW(opal_pmix20_jobid_trkr_t);
    (void)strncpy(jptr->nspace, nspace, PMIX_MAX_NSLEN);
    jptr->jobid = jobid;
    opal_list_append(&mca_pmix_ext20_component.jobids, &jptr->super);
}

static void completion_handler(int status, void *cbdata)
{
    opal_pmix20_event_chain_t *chain = (opal_pmix20_event_chain_t*)cbdata;
    if (NULL != chain->info) {
        OPAL_LIST_RELEASE(chain->info);
    }
}

static void progress_local_event_hdlr(int status,
                                      opal_list_t *results,
                                      opal_pmix_op_cbfunc_t cbfunc, void *thiscbdata,
                                      void *notification_cbdata)
{
    opal_pmix20_event_chain_t *chain = (opal_pmix20_event_chain_t*)notification_cbdata;
    size_t n;
    opal_list_item_t *nxt;
    opal_pmix20_single_event_t *sing;
    opal_pmix20_multi_event_t *multi;
    opal_pmix20_default_event_t *def;

    /* if any results were provided, then add them here */
    if (NULL != results) {
        while (NULL != (nxt = opal_list_remove_first(results))) {
            opal_list_append(results, nxt);
        }
    }

    /* if the caller indicates that the chain is completed, then stop here */
    if (OPAL_ERR_HANDLERS_COMPLETE == status) {
        goto complete;
    }

    /* see if we need to continue, starting with the single code events */
    if (NULL != chain->sing) {
        /* the last handler was for a single code - see if there are
         * any others that match this event */
        while (opal_list_get_end(&mca_pmix_ext20_component.single_events) != (nxt = opal_list_get_next(&chain->sing->super))) {
            sing = (opal_pmix20_single_event_t*)nxt;
            if (sing->code == chain->status) {
                OBJ_RETAIN(chain);
                chain->sing = sing;
                sing->handler(chain->status, &chain->source,
                              chain->info, &chain->results,
                              progress_local_event_hdlr, (void*)chain);
                goto complete;
            }
        }
        /* if we get here, then there are no more single code
         * events that match */
        chain->sing = NULL;
        /* pickup the beginning of the multi-code event list */
        if (0 < opal_list_get_size(&mca_pmix_ext20_component.multi_events)) {
            chain->multi = (opal_pmix20_multi_event_t*)opal_list_get_begin(&mca_pmix_ext20_component.multi_events);
        }
    }

    /* see if we need to continue with the multi code events */
    if (NULL != chain->multi) {
        while (opal_list_get_end(&mca_pmix_ext20_component.multi_events) != (nxt = opal_list_get_next(&chain->multi->super))) {
            multi = (opal_pmix20_multi_event_t*)nxt;
            for (n=0; n < multi->ncodes; n++) {
                if (multi->codes[n] == chain->status) {
                    /* found it - invoke the handler, pointing its
                     * callback function to our progression function */
                    OBJ_RETAIN(chain);
                    chain->multi = multi;
                    multi->handler(chain->status, &chain->source,
                                   chain->info, &chain->results,
                                   progress_local_event_hdlr, (void*)chain);
                    goto complete;
                }
            }
        }
        /* if we get here, then there are no more multi-mode
         * events that match */
        chain->multi = NULL;
        /* pickup the beginning of the default event list */
        if (0 < opal_list_get_size(&mca_pmix_ext20_component.default_events)) {
            chain->def = (opal_pmix20_default_event_t*)opal_list_get_begin(&mca_pmix_ext20_component.default_events);
        }
    }

    /* if they didn't want it to go to a default handler, then we are done */
    if (chain->nondefault) {
        goto complete;
    }

    if (NULL != chain->def) {
        if (opal_list_get_end(&mca_pmix_ext20_component.default_events) != (nxt = opal_list_get_next(&chain->def->super))) {
            def = (opal_pmix20_default_event_t*)nxt;
            OBJ_RETAIN(chain);
            chain->def = def;
            def->handler(chain->status, &chain->source,
                         chain->info, &chain->results,
                         progress_local_event_hdlr, (void*)chain);
        }
    }

  complete:
    /* we still have to call their final callback */
    if (NULL != chain->final_cbfunc) {
        chain->final_cbfunc(OPAL_SUCCESS, chain->final_cbdata);
    }
    /* maintain acctng */
    OBJ_RELEASE(chain);
    /* let the caller know that we are done with their callback */
    if (NULL != cbfunc) {
        cbfunc(OPAL_SUCCESS, thiscbdata);
    }
}

static void _event_hdlr(int sd, short args, void *cbdata)
{
    pmix20_threadshift_t *cd = (pmix20_threadshift_t*)cbdata;
    size_t n;
    opal_pmix20_event_chain_t *chain;
    opal_pmix20_single_event_t *sing;
    opal_pmix20_multi_event_t *multi;
    opal_pmix20_default_event_t *def;

    opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                        "%s RECEIVED NOTIFICATION OF STATUS %d",
                        OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), cd->status);

    chain = OBJ_NEW(opal_pmix20_event_chain_t);
    /* point it at our final callback */
    chain->final_cbfunc = completion_handler;
    chain->final_cbdata = chain;

    /* carry across provided info */
    chain->status = cd->status;
    chain->source = cd->pname;
    chain->info = cd->info;
    chain->nondefault = cd->nondefault;

    /* cycle thru the single-event registrations first */
    OPAL_LIST_FOREACH(sing, &mca_pmix_ext20_component.single_events, opal_pmix20_single_event_t) {
        if (sing->code == chain->status) {
            /* found it - invoke the handler, pointing its
             * callback function to our progression function */
            OBJ_RETAIN(chain);
            chain->sing = sing;
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s CALLING SINGLE EVHDLR",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
            sing->handler(chain->status, &chain->source,
                          chain->info, &chain->results,
                          progress_local_event_hdlr, (void*)chain);
            return;
        }
    }

    /* if we didn't find any match in the single-event registrations,
     * then cycle thru the multi-event registrations next */
    OPAL_LIST_FOREACH(multi, &mca_pmix_ext20_component.multi_events, opal_pmix20_multi_event_t) {
        for (n=0; n < multi->ncodes; n++) {
            if (multi->codes[n] == chain->status) {
                /* found it - invoke the handler, pointing its
                 * callback function to our progression function */
                OBJ_RETAIN(chain);
                chain->multi = multi;
                opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                    "%s CALLING MULTI EVHDLR",
                                    OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
                multi->handler(chain->status, &chain->source,
                               chain->info, &chain->results,
                               progress_local_event_hdlr, (void*)chain);
                return;
            }
        }
    }

      /* if they didn't want it to go to a default handler, then we are done */
    if (chain->nondefault) {
        /* if we get here, then we need to cache this event in case they
         * register for it later - we cannot lose individual events */
        opal_list_append(&mca_pmix_ext20_component.cache, &chain->super);
        return;
    }

    /* we are done with the threadshift caddy */
    OBJ_RELEASE(cd);

    /* finally, pass it to any default handlers */
    if (0 < opal_list_get_size(&mca_pmix_ext20_component.default_events)) {
        def = (opal_pmix20_default_event_t*)opal_list_get_first(&mca_pmix_ext20_component.default_events);
        OBJ_RETAIN(chain);
        chain->def = def;
        opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                            "%s CALLING DEFAULT EVHDLR",
                            OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
        def->handler(chain->status, &chain->source,
                     chain->info, &chain->results,
                     progress_local_event_hdlr, (void*)chain);
        return;
    }

    /* we still have to call their final callback */
    if (NULL != chain->final_cbfunc) {
        chain->final_cbfunc(PMIX_SUCCESS, chain->final_cbdata);
    }
    return;
}

/* this function will be called by the PMIx client library
 * whenever it receives notification of an event. The
 * notification can come from an ORTE daemon (when launched
 * by mpirun), directly from a RM (when direct launched), or
 * from another process (via the local daemon).
 * The call will occur in the PMIx event base */
void pmix20_event_hdlr(size_t evhdlr_registration_id,
                       pmix_status_t status, const pmix_proc_t *source,
                       pmix_info_t info[], size_t ninfo,
                       pmix_info_t results[], size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    pmix20_threadshift_t *cd;
    int rc;
    opal_value_t *iptr;
    size_t n;

    /* this is in the PMIx local thread - need to threadshift to
     * our own thread as we will be accessing framework-global
     * lists and objects */

     opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                         "%s RECEIVED NOTIFICATION OF STATUS %d",
                         OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), status);

    cd = OBJ_NEW(pmix20_threadshift_t);

    /* convert the incoming status */
    cd->status = pmix20_convert_rc(status);
    opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                        "%s CONVERTED STATUS %d TO STATUS %d",
                        OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), status, cd->status);
    /* convert the nspace/rank to an opal_process_name_t */
    if (NULL == source) {
        cd->pname.jobid = OPAL_NAME_INVALID->jobid;
        cd->pname.vpid = OPAL_NAME_INVALID->vpid;
    } else {
        if (OPAL_SUCCESS != (rc = opal_convert_string_to_jobid(&cd->pname.jobid, source->nspace))) {
            OPAL_ERROR_LOG(rc);
            OBJ_RELEASE(cd);
            return;
        }
        cd->pname.vpid = source->rank;
    }

    /* convert the array of info */
    if (NULL != info) {
        cd->info = OBJ_NEW(opal_list_t);
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_NON_DEFAULT, PMIX_MAX_KEYLEN)) {
                cd->nondefault = true;
            }
            iptr = OBJ_NEW(opal_value_t);
            iptr->key = strdup(info[n].key);
            pmix20_value_unload(iptr, &info[n].value);
            opal_list_append(cd->info, &iptr->super);
        }
    }
    /* now push it into the local thread */
    event_assign(&cd->ev, opal_pmix_base.evbase,
                 -1, EV_WRITE, _event_hdlr, cd);
    event_active(&cd->ev, EV_WRITE, 1);

    /* we don't need any of the data they provided,
     * so let them go */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

pmix_status_t pmix20_convert_opalrc(int rc)
{
    switch (rc) {
    case OPAL_ERR_DEBUGGER_RELEASE:
        return PMIX_ERR_DEBUGGER_RELEASE;

    case OPAL_ERR_NOT_IMPLEMENTED:
    case OPAL_ERR_NOT_SUPPORTED:
        return PMIX_ERR_NOT_SUPPORTED;

    case OPAL_ERR_NOT_FOUND:
        return PMIX_ERR_NOT_FOUND;

    case OPAL_ERR_PERM:
    case OPAL_ERR_UNREACH:
    case OPAL_ERR_SERVER_NOT_AVAIL:
        return PMIX_ERR_UNREACH;

    case OPAL_ERR_BAD_PARAM:
        return PMIX_ERR_BAD_PARAM;

    case OPAL_ERR_OUT_OF_RESOURCE:
        return PMIX_ERR_OUT_OF_RESOURCE;

    case OPAL_ERR_DATA_VALUE_NOT_FOUND:
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;

    case OPAL_ERR_TIMEOUT:
        return PMIX_ERR_TIMEOUT;

    case OPAL_ERR_WOULD_BLOCK:
        return PMIX_ERR_WOULD_BLOCK;

    case OPAL_EXISTS:
        return PMIX_EXISTS;

    case OPAL_ERROR:
        return PMIX_ERROR;
    case OPAL_SUCCESS:
        return PMIX_SUCCESS;
    default:
        return PMIX_ERROR;
    }
}

int pmix20_convert_rc(pmix_status_t rc)
{
    switch (rc) {
    case PMIX_ERR_DEBUGGER_RELEASE:
        return OPAL_ERR_DEBUGGER_RELEASE;

    case PMIX_ERR_NOT_SUPPORTED:
        return OPAL_ERR_NOT_SUPPORTED;

    case PMIX_ERR_NOT_FOUND:
        return OPAL_ERR_NOT_FOUND;

    case PMIX_ERR_OUT_OF_RESOURCE:
        return OPAL_ERR_OUT_OF_RESOURCE;

    case PMIX_ERR_INIT:
        return OPAL_ERROR;

    case PMIX_ERR_BAD_PARAM:
        return OPAL_ERR_BAD_PARAM;

    case PMIX_ERR_UNREACH:
    case PMIX_ERR_NO_PERMISSIONS:
        return OPAL_ERR_UNREACH;

    case PMIX_ERR_TIMEOUT:
        return OPAL_ERR_TIMEOUT;

    case PMIX_ERR_WOULD_BLOCK:
        return OPAL_ERR_WOULD_BLOCK;

    case PMIX_ERR_LOST_CONNECTION_TO_SERVER:
    case PMIX_ERR_LOST_PEER_CONNECTION:
    case PMIX_ERR_LOST_CONNECTION_TO_CLIENT:
        return OPAL_ERR_COMM_FAILURE;

    case PMIX_EXISTS:
        return OPAL_EXISTS;

    case PMIX_ERROR:
        return OPAL_ERROR;
    case PMIX_SUCCESS:
        return OPAL_SUCCESS;
    default:
        return OPAL_ERROR;
    }
}

pmix_scope_t pmix20_convert_opalscope(opal_pmix_scope_t scope) {
    switch(scope) {
    case OPAL_PMIX_LOCAL:
        return PMIX_LOCAL;
    case OPAL_PMIX_REMOTE:
        return PMIX_REMOTE;
    case OPAL_PMIX_GLOBAL:
        return PMIX_GLOBAL;
    default:
        return PMIX_SCOPE_UNDEF;
    }
}

pmix_data_range_t pmix20_convert_opalrange(opal_pmix_data_range_t range) {
    switch(range) {
    case OPAL_PMIX_RANGE_UNDEF:
        return PMIX_RANGE_UNDEF;
    case OPAL_PMIX_RANGE_LOCAL:
        return PMIX_RANGE_LOCAL;
    case OPAL_PMIX_RANGE_NAMESPACE:
        return PMIX_RANGE_NAMESPACE;
    case OPAL_PMIX_RANGE_SESSION:
        return PMIX_RANGE_SESSION;
    case OPAL_PMIX_RANGE_GLOBAL:
        return PMIX_RANGE_GLOBAL;
    case OPAL_PMIX_RANGE_CUSTOM:
        return PMIX_RANGE_CUSTOM;
    default:
        return PMIX_SCOPE_UNDEF;
    }
}

opal_pmix_data_range_t pmix20_convert_range(pmix_data_range_t range) {
    switch(range) {
    case PMIX_RANGE_UNDEF:
        return OPAL_PMIX_RANGE_UNDEF;
    case PMIX_RANGE_LOCAL:
        return OPAL_PMIX_RANGE_LOCAL;
    case PMIX_RANGE_NAMESPACE:
        return OPAL_PMIX_RANGE_NAMESPACE;
    case PMIX_RANGE_SESSION:
        return OPAL_PMIX_RANGE_SESSION;
    case PMIX_RANGE_GLOBAL:
        return OPAL_PMIX_RANGE_GLOBAL;
    case PMIX_RANGE_CUSTOM:
        return OPAL_PMIX_RANGE_CUSTOM;
    default:
        return OPAL_PMIX_SCOPE_UNDEF;
    }
}


void pmix20_value_load(pmix_value_t *v,
                       opal_value_t *kv)
{
    size_t n;
    char nspace[PMIX_MAX_NSLEN + 1];

    switch(kv->type) {
        case OPAL_UNDEF:
            v->type = PMIX_UNDEF;
            break;
        case OPAL_BOOL:
            v->type = PMIX_BOOL;
            memcpy(&(v->data.flag), &kv->data.flag, 1);
            break;
        case OPAL_BYTE:
            v->type = PMIX_BYTE;
            memcpy(&(v->data.byte), &kv->data.byte, 1);
            break;
        case OPAL_STRING:
            v->type = PMIX_STRING;
            if (NULL != kv->data.string) {
                v->data.string = strdup(kv->data.string);
            } else {
                v->data.string = NULL;
            }
            break;
        case OPAL_SIZE:
            v->type = PMIX_SIZE;
            v->data.size = (size_t)kv->data.size;
            break;
        case OPAL_PID:
            v->type = PMIX_PID;
            memcpy(&(v->data.pid), &kv->data.pid, sizeof(pid_t));
            break;
        case OPAL_INT:
            v->type = PMIX_INT;
            memcpy(&(v->data.integer), &kv->data.integer, sizeof(int));
            break;
        case OPAL_INT8:
            v->type = PMIX_INT8;
            memcpy(&(v->data.int8), &kv->data.int8, 1);
            break;
        case OPAL_INT16:
            v->type = PMIX_INT16;
            memcpy(&(v->data.int16), &kv->data.int16, 2);
            break;
        case OPAL_INT32:
            v->type = PMIX_INT32;
            memcpy(&(v->data.int32), &kv->data.int32, 4);
            break;
        case OPAL_INT64:
            v->type = PMIX_INT64;
            memcpy(&(v->data.int64), &kv->data.int64, 8);
            break;
        case OPAL_UINT:
            v->type = PMIX_UINT;
            memcpy(&(v->data.uint), &kv->data.uint, sizeof(int));
            break;
        case OPAL_UINT8:
            v->type = PMIX_UINT8;
            memcpy(&(v->data.uint8), &kv->data.uint8, 1);
            break;
        case OPAL_UINT16:
            v->type = PMIX_UINT16;
            memcpy(&(v->data.uint16), &kv->data.uint16, 2);
            break;
        case OPAL_UINT32:
            v->type = PMIX_UINT32;
            memcpy(&(v->data.uint32), &kv->data.uint32, 4);
            break;
        case OPAL_UINT64:
            v->type = PMIX_UINT64;
            memcpy(&(v->data.uint64), &kv->data.uint64, 8);
            break;
        case OPAL_FLOAT:
            v->type = PMIX_FLOAT;
            memcpy(&(v->data.fval), &kv->data.fval, sizeof(float));
            break;
        case OPAL_DOUBLE:
            v->type = PMIX_DOUBLE;
            memcpy(&(v->data.dval), &kv->data.dval, sizeof(double));
            break;
        case OPAL_TIMEVAL:
            v->type = PMIX_TIMEVAL;
            memcpy(&(v->data.tv), &kv->data.tv, sizeof(struct timeval));
            break;
        case OPAL_BYTE_OBJECT:
            v->type = PMIX_BYTE_OBJECT;
            if (NULL != kv->data.bo.bytes) {
                v->data.bo.bytes = (char*)malloc(kv->data.bo.size);
                memcpy(v->data.bo.bytes, kv->data.bo.bytes, kv->data.bo.size);
                v->data.bo.size = (size_t)kv->data.bo.size;
            } else {
                v->data.bo.bytes = NULL;
                v->data.bo.size = 0;
            }
            break;
        case OPAL_UINT32_ARRAY:
            /* an array of 32-bit jobids */
            v->type = PMIX_INFO_ARRAY;
            v->data.array.size = kv->data.uint32_array.size;
            if (0 < v->data.array.size) {
                PMIX_INFO_CREATE(v->data.array.array, v->data.array.size);
                for (n=0; n < v->data.array.size; n++) {
                    v->data.array.array[n].value.type = PMIX_STRING;
                    (void)opal_snprintf_jobid(nspace, PMIX_MAX_NSLEN, kv->data.uint32_array.data[n]);
                    v->data.array.array[n].value.data.string = strdup(nspace);
                }
            }
            break;
        default:
            /* silence warnings */
            break;
    }
}

int pmix20_value_unload(opal_value_t *kv,
                       const pmix_value_t *v)
{
    int rc=OPAL_SUCCESS;


    switch(v->type) {
    case PMIX_UNDEF:
        kv->type = OPAL_UNDEF;
        break;
    case PMIX_BOOL:
        kv->type = OPAL_BOOL;
        memcpy(&kv->data.flag, &(v->data.flag), 1);
        break;
    case PMIX_BYTE:
        kv->type = OPAL_BYTE;
        memcpy(&kv->data.byte, &(v->data.byte), 1);
        break;
    case PMIX_STRING:
        kv->type = OPAL_STRING;
        if (NULL != v->data.string) {
            kv->data.string = strdup(v->data.string);
        }
        break;
    case PMIX_SIZE:
        kv->type = OPAL_SIZE;
        kv->data.size = (int)v->data.size;
        break;
    case PMIX_PID:
        kv->type = OPAL_PID;
        memcpy(&kv->data.pid, &(v->data.pid), sizeof(pid_t));
        break;
    case PMIX_INT:
        kv->type = OPAL_INT;
        memcpy(&kv->data.integer, &(v->data.integer), sizeof(int));
        break;
    case PMIX_INT8:
        kv->type = OPAL_INT8;
        memcpy(&kv->data.int8, &(v->data.int8), 1);
        break;
    case PMIX_INT16:
        kv->type = OPAL_INT16;
        memcpy(&kv->data.int16, &(v->data.int16), 2);
        break;
    case PMIX_INT32:
        kv->type = OPAL_INT32;
        memcpy(&kv->data.int32, &(v->data.int32), 4);
        break;
    case PMIX_INT64:
        kv->type = OPAL_INT64;
        memcpy(&kv->data, &(v->data.int64), 8);
        break;
    case PMIX_UINT:
        kv->type = OPAL_UINT;
        memcpy(&kv->data, &(v->data.uint), sizeof(int));
        break;
    case PMIX_UINT8:
        kv->type = OPAL_UINT8;
        memcpy(&kv->data, &(v->data.uint8), 1);
        break;
    case PMIX_UINT16:
        kv->type = OPAL_UINT16;
        memcpy(&kv->data, &(v->data.uint16), 2);
        break;
    case PMIX_UINT32:
        kv->type = OPAL_UINT32;
        memcpy(&kv->data, &(v->data.uint32), 4);
        break;
    case PMIX_UINT64:
        kv->type = OPAL_UINT64;
        memcpy(&kv->data, &(v->data.uint64), 8);
        break;
    case PMIX_FLOAT:
        kv->type = OPAL_FLOAT;
        memcpy(&kv->data, &(v->data.fval), sizeof(float));
        break;
    case PMIX_DOUBLE:
        kv->type = OPAL_DOUBLE;
        memcpy(&kv->data, &(v->data.dval), sizeof(double));
        break;
    case PMIX_TIMEVAL:
        kv->type = OPAL_TIMEVAL;
        memcpy(&kv->data, &(v->data.tv), sizeof(struct timeval));
        break;
    case PMIX_BYTE_OBJECT:
        kv->type = OPAL_BYTE_OBJECT;
        if (NULL != v->data.bo.bytes && 0 < v->data.bo.size) {
            kv->data.bo.bytes = (uint8_t*)malloc(v->data.bo.size);
            memcpy(kv->data.bo.bytes, v->data.bo.bytes, v->data.bo.size);
            kv->data.bo.size = (int)v->data.bo.size;
        } else {
            kv->data.bo.bytes = NULL;
            kv->data.bo.size = 0;
        }
        break;
    default:
        /* silence warnings */
        rc = OPAL_ERROR;
        break;
    }
    return rc;
}

static void _reg_hdlr(int sd, short args, void *cbdata)
{
    pmix20_threadshift_t *cd = (pmix20_threadshift_t*)cbdata;
    opal_pmix20_event_chain_t *chain;
    opal_pmix20_single_event_t *sing = NULL;
    opal_pmix20_multi_event_t *multi = NULL;
    opal_pmix20_default_event_t *def = NULL;
    opal_value_t *kv;
    int i;
    bool prepend = false;
    size_t n;

    opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                        "%s REGISTER HANDLER CODES %s",
                        OPAL_NAME_PRINT(OPAL_PROC_MY_NAME),
                        (NULL == cd->event_codes) ? "NULL" : "NON-NULL");

    if (NULL != cd->info) {
        OPAL_LIST_FOREACH(kv, cd->info, opal_value_t) {
            if (0 == strcmp(kv->key, OPAL_PMIX_EVENT_ORDER_PREPEND)) {
                prepend = true;
                break;
            }
        }
    }

    if (NULL == cd->event_codes) {
        /* this is a default handler */
        def = OBJ_NEW(opal_pmix20_default_event_t);
        def->handler = cd->evhandler;
        def->index = mca_pmix_ext20_component.evindex;
        if (prepend) {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s PREPENDING TO DEFAULT EVENTS",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
            opal_list_prepend(&mca_pmix_ext20_component.default_events, &def->super);
        } else {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s APPENDING TO DEFAULT EVENTS",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
            opal_list_append(&mca_pmix_ext20_component.default_events, &def->super);
        }
    } else if (1 == opal_list_get_size(cd->event_codes)) {
        /* single handler */
        sing = OBJ_NEW(opal_pmix20_single_event_t);
        kv = (opal_value_t*)opal_list_get_first(cd->event_codes);
        sing->code = kv->data.integer;
        sing->index = mca_pmix_ext20_component.evindex;
        sing->handler = cd->evhandler;
        if (prepend) {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s PREPENDING TO SINGLE EVENTS WITH CODE %d",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), sing->code);
            opal_list_prepend(&mca_pmix_ext20_component.single_events, &sing->super);
        } else {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s APPENDING TO SINGLE EVENTS WITH CODE %d",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), sing->code);
            opal_list_append(&mca_pmix_ext20_component.single_events, &sing->super);
        }
    } else {
        multi = OBJ_NEW(opal_pmix20_multi_event_t);
        multi->ncodes = opal_list_get_size(cd->event_codes);
        multi->codes = (int*)malloc(multi->ncodes * sizeof(int));
        i=0;
        OPAL_LIST_FOREACH(kv, cd->event_codes, opal_value_t) {
            multi->codes[i] = kv->data.integer;
            ++i;
        }
        multi->index = mca_pmix_ext20_component.evindex;
        multi->handler = cd->evhandler;
        if (prepend) {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s PREPENDING TO MULTI EVENTS",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
            opal_list_prepend(&mca_pmix_ext20_component.multi_events, &multi->super);
        } else {
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "%s APPENDING TO MULTI EVENTS",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME));
            opal_list_append(&mca_pmix_ext20_component.multi_events, &multi->super);
        }
    }

    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(OPAL_SUCCESS, mca_pmix_ext20_component.evindex, cd->cbdata);
    }
    mca_pmix_ext20_component.evindex++;

    /* check if any matching notifications have been cached - only nondefault
     * events will have been cached*/
    if (NULL == def) {
        /* check single code registrations */
        if (NULL != sing) {
            OPAL_LIST_FOREACH(chain, &mca_pmix_ext20_component.cache, opal_pmix20_event_chain_t) {
                if (sing->code == chain->status) {
                    opal_list_remove_item(&mca_pmix_ext20_component.cache, &chain->super);
                    chain->sing = sing;
                    sing->handler(chain->status, &chain->source,
                                  chain->info, &chain->results,
                                  progress_local_event_hdlr, (void*)chain);
                    OBJ_RELEASE(cd);
                    return;
                }
            }
        } else if (NULL != multi) {
            /* check for multi code registrations */
            OPAL_LIST_FOREACH(chain, &mca_pmix_ext20_component.cache, opal_pmix20_event_chain_t) {
                for (n=0; n < multi->ncodes; n++) {
                    if (multi->codes[n] == chain->status) {
                        opal_list_remove_item(&mca_pmix_ext20_component.cache, &chain->super);
                        chain->multi = multi;
                        multi->handler(chain->status, &chain->source,
                                      chain->info, &chain->results,
                                      progress_local_event_hdlr, (void*)chain);
                        OBJ_RELEASE(cd);
                        return;
                    }
                }
            }
        }
    }

    OBJ_RELEASE(cd);
    return;
}
static void register_handler(opal_list_t *event_codes,
                             opal_list_t *info,
                             opal_pmix_notification_fn_t evhandler,
                             opal_pmix_evhandler_reg_cbfunc_t cbfunc,
                             void *cbdata)
{
    /* we must threadshift this request as we might not be in an event
     * and we are going to access framework-global lists/objects */

    OPAL_PMIX_THREADSHIFT(event_codes, info, evhandler, _reg_hdlr, cbfunc, cbdata);
    return;
}

static void _dereg_hdlr(int sd, short args, void *cbdata)
{
    pmix20_threadshift_t *cd = (pmix20_threadshift_t*)cbdata;
    opal_pmix20_single_event_t *sing;
    opal_pmix20_multi_event_t *multi;
    opal_pmix20_default_event_t *def;

    /* check the single events first */
    OPAL_LIST_FOREACH(sing, &mca_pmix_ext20_component.single_events, opal_pmix20_single_event_t) {
        if (cd->handler == sing->index) {
            opal_list_remove_item(&mca_pmix_ext20_component.single_events, &sing->super);
            OBJ_RELEASE(sing);
            goto release;
        }
    }
    /* check multi events */
    OPAL_LIST_FOREACH(multi, &mca_pmix_ext20_component.multi_events, opal_pmix20_multi_event_t) {
        if (cd->handler == multi->index) {
            opal_list_remove_item(&mca_pmix_ext20_component.multi_events, &multi->super);
            OBJ_RELEASE(multi);
            goto release;
        }
    }
    /* check default events */
    OPAL_LIST_FOREACH(def, &mca_pmix_ext20_component.default_events, opal_pmix20_default_event_t) {
        if (cd->handler == def->index) {
            opal_list_remove_item(&mca_pmix_ext20_component.default_events, &def->super);
            OBJ_RELEASE(def);
            break;
        }
    }

  release:
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(OPAL_SUCCESS, cd->cbdata);
    }
    OBJ_RELEASE(cd);
}

static void deregister_handler(size_t evhandler,
                               opal_pmix_op_cbfunc_t cbfunc,
                               void *cbdata)
{
    /* we must threadshift this request as we might not be in an event
     * and we are going to access framework-global lists/objects */
    OPAL_PMIX_OP_THREADSHIFT(evhandler, _dereg_hdlr, cbfunc, cbdata);
    return;
}

static void _notify_event(int sd, short args, void *cbdata)
{
    pmix20_threadshift_t *cd = (pmix20_threadshift_t*)cbdata;
    size_t i;
    opal_pmix20_single_event_t *sing;
    opal_pmix20_multi_event_t *multi;
    opal_pmix20_default_event_t *def;
    opal_pmix20_event_chain_t *chain;

    /* check the single events first */
    OPAL_LIST_FOREACH(sing, &mca_pmix_ext20_component.single_events, opal_pmix20_single_event_t) {
        if (cd->status == sing->code) {
            /* found it - invoke the handler, pointing its
             * callback function to our progression function */
            chain = OBJ_NEW(opal_pmix20_event_chain_t);
            chain->status = cd->status;
            chain->range = pmix20_convert_opalrange(cd->range);
            chain->source = *(cd->source);
            chain->info = cd->info;
            chain->final_cbfunc = cd->opcbfunc;
            chain->final_cbdata = cd->cbdata;
            chain->sing = sing;
            opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                "[%s] CALLING SINGLE EVHDLR FOR STATUS %d",
                                OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), chain->status);
            sing->handler(chain->status, &chain->source,
                          chain->info, &chain->results,
                          progress_local_event_hdlr, (void*)chain);
            OBJ_RELEASE(cd);
            return;
        }
    }
    /* check multi events */
    OPAL_LIST_FOREACH(multi, &mca_pmix_ext20_component.multi_events, opal_pmix20_multi_event_t) {
        for (i=0; i < multi->ncodes; i++) {
            if (cd->status == multi->codes[i]) {
                /* found it - invoke the handler, pointing its
                 * callback function to our progression function */
                chain = OBJ_NEW(opal_pmix20_event_chain_t);
                chain->status = cd->status;
                chain->range = pmix20_convert_opalrange(cd->range);
                chain->source = *(cd->source);
                chain->info = cd->info;
                chain->final_cbfunc = cd->opcbfunc;
                chain->final_cbdata = cd->cbdata;
                chain->multi = multi;
                opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                                    "[%s] CALLING MULTI EVHDLR FOR STATUS %d",
                                    OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), chain->status);
                multi->handler(chain->status, &chain->source,
                               chain->info, &chain->results,
                               progress_local_event_hdlr, (void*)chain);
                OBJ_RELEASE(cd);
                return;
            }
        }
    }
    /* check default events */
    if (0 < opal_list_get_size(&mca_pmix_ext20_component.default_events)) {
        def = (opal_pmix20_default_event_t*)opal_list_get_first(&mca_pmix_ext20_component.default_events);
        chain = OBJ_NEW(opal_pmix20_event_chain_t);
        chain->status = cd->status;
        chain->range = pmix20_convert_opalrange(cd->range);
        chain->source = *(cd->source);
        chain->info = cd->info;
        chain->final_cbfunc = cd->opcbfunc;
        chain->final_cbdata = cd->cbdata;
        chain->def = def;
        opal_output_verbose(2, opal_pmix_base_framework.framework_output,
                            "[%s] CALLING DEFAULT EVHDLR FOR STATUS %d",
                            OPAL_NAME_PRINT(OPAL_PROC_MY_NAME), chain->status);
        def->handler(chain->status, &chain->source,
                     chain->info, &chain->results,
                     progress_local_event_hdlr, (void*)chain);
        OBJ_RELEASE(cd);
        return;
    }

    /* if we get here, then there are no registered event handlers */
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(OPAL_ERR_NOT_FOUND, cd->cbdata);
    }
    OBJ_RELEASE(cd);
    return;
}

static int notify_event(int status,
                        const opal_process_name_t *source,
                        opal_pmix_data_range_t range,
                        opal_list_t *info,
                        opal_pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    /* we must threadshift this request as we might not be in an event
     * and we are going to access framework-global lists/objects */
    OPAL_PMIX_NOTIFY_THREADSHIFT(status, source, range, info, _notify_event, cbfunc, cbdata);
    return OPAL_SUCCESS;
}

/****  INSTANTIATE INTERNAL CLASSES  ****/
OBJ_CLASS_INSTANCE(opal_pmix20_jobid_trkr_t,
                   opal_list_item_t,
                   NULL, NULL);

OBJ_CLASS_INSTANCE(opal_pmix20_single_event_t,
                   opal_list_item_t,
                   NULL, NULL);

static void mtevcon(opal_pmix20_multi_event_t *p)
{
    p->codes = NULL;
    p->ncodes = 0;
}
static void mtevdes(opal_pmix20_multi_event_t *p)
{
    if (NULL != p->codes) {
        free(p->codes);
    }
}
OBJ_CLASS_INSTANCE(opal_pmix20_multi_event_t,
                   opal_list_item_t,
                   mtevcon, mtevdes);

OBJ_CLASS_INSTANCE(opal_pmix20_default_event_t,
                   opal_list_item_t,
                   NULL, NULL);

static void chcon(opal_pmix20_event_chain_t *p)
{
    p->nondefault = false;
    p->info = NULL;
    OBJ_CONSTRUCT(&p->results, opal_list_t);
    p->sing = NULL;
    p->multi = NULL;
    p->def = NULL;
    p->final_cbfunc = NULL;
    p->final_cbdata = NULL;
}
static void chdes(opal_pmix20_event_chain_t *p)
{
    OPAL_LIST_DESTRUCT(&p->results);
}
OBJ_CLASS_INSTANCE(opal_pmix20_event_chain_t,
                   opal_list_item_t,
                   chcon, chdes);

static void opcon(pmix20_opcaddy_t *p)
{
    memset(&p->p, 0, sizeof(pmix_proc_t));
    p->procs = NULL;
    p->nprocs = 0;
    p->error_procs = NULL;
    p->nerror_procs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->apps = NULL;
    p->sz = 0;
    p->active = false;
    p->opcbfunc = NULL;
    p->mdxcbfunc = NULL;
    p->valcbfunc = NULL;
    p->lkcbfunc = NULL;
    p->spcbfunc = NULL;
    p->cbdata = NULL;
}
static void opdes(pmix20_opcaddy_t *p)
{
    if (NULL != p->procs) {
        PMIX_PROC_FREE(p->procs, p->nprocs);
    }
    if (NULL != p->error_procs) {
        PMIX_PROC_FREE(p->error_procs, p->nerror_procs);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->sz);
    }
    if (NULL != p->apps) {
        PMIX_APP_FREE(p->apps, p->sz);
    }
}
OBJ_CLASS_INSTANCE(pmix20_opcaddy_t,
                   opal_object_t,
                   opcon, opdes);

static void ocadcon(pmix20_opalcaddy_t *p)
{
    OBJ_CONSTRUCT(&p->procs, opal_list_t);
    OBJ_CONSTRUCT(&p->info, opal_list_t);
    OBJ_CONSTRUCT(&p->apps, opal_list_t);
    p->opcbfunc = NULL;
    p->dmdxfunc = NULL;
    p->mdxcbfunc = NULL;
    p->lkupcbfunc = NULL;
    p->spwncbfunc = NULL;
    p->cbdata = NULL;
    p->odmdxfunc = NULL;
#if HAVE_PMIX_QUERY_FUNCTION
    p->infocbfunc = NULL;
    p->toolcbfunc = NULL;
#endif
    p->ocbdata = NULL;
}
static void ocaddes(pmix20_opalcaddy_t *p)
{
    OPAL_LIST_DESTRUCT(&p->procs);
    OPAL_LIST_DESTRUCT(&p->info);
    OPAL_LIST_DESTRUCT(&p->apps);
}
OBJ_CLASS_INSTANCE(pmix20_opalcaddy_t,
                   opal_object_t,
                   ocadcon, ocaddes);

static void tscon(pmix20_threadshift_t *p)
{
    p->active = false;
    p->source = NULL;
    p->event_codes = NULL;
    p->info = NULL;
    p->evhandler = NULL;
    p->cbfunc = NULL;
    p->opcbfunc = NULL;
    p->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(pmix20_threadshift_t,
                   opal_object_t,
                   tscon, NULL);

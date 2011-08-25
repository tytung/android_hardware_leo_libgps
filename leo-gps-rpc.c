/******************************************************************************
 * RPC Client of GPS HAL (hardware abstraction layer) for HD2/Leo
 * 
 * leo-gps-rpc.c
 * 
 * Copyright (C) 2009-2010 The XDAndroid Project
 * Copyright (C) 2010      dan1j3l @ xda-developers
 * Copyright (C) 2011      tytung  @ xda-developers
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <librpc/rpc/rpc.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <librpc/rpc/rpc_router_ioctl.h>
#include <pthread.h>
#include <cutils/log.h>
#include <gps.h>

#define  LOG_TAG  "gps_leo_rpc"

#define  ENABLE_NMEA 1

#define  DUMP_DATA  0
#define  GPS_DEBUG  0

#if GPS_DEBUG
#  define  D(...)   LOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif

typedef struct registered_server_struct {
    /* MUST BE AT OFFSET ZERO!  The client code assumes this when it overwrites
     * the XDR for server entries which represent a callback client.  Those
     * server entries do not have their own XDRs.
     */
    XDR *xdr;
    /* Because the xdr is NULL for callback clients (as opposed to true
     * servers), we keep track of the program number and version number in this
     * structure as well.
     */
    rpcprog_t x_prog; /* program number */
    rpcvers_t x_vers; /* program version */

    int active;
    struct registered_server_struct *next;
    SVCXPRT *xprt;
    __dispatch_fn_t dispatch;
} registered_server;

struct SVCXPRT {
    fd_set fdset;
    int max_fd;
    pthread_attr_t thread_attr;
    pthread_t  svc_thread;
    pthread_mutexattr_t lock_attr;
    pthread_mutex_t lock;
    registered_server *servers;
    volatile int num_servers;
};

#define SEND_VAL(x) do { \
    val=x;\
    XDR_SEND_UINT32(clnt, &val);\
} while(0);

static uint32_t client_IDs[16];//highest known value is 0xb
static uint32_t no_fix=1;
#if ENABLE_NMEA
static uint32_t use_nmea=1;
#else
static uint32_t use_nmea=0;
#endif
static struct CLIENT *_clnt;
static struct timeval timeout;
static SVCXPRT *_svc;

static uint8_t CHECKED[4] = {0};
static uint8_t XTRA_AUTO_DOWNLOAD_ENABLED = 0;
static uint8_t XTRA_DOWNLOAD_INTERVAL = 24;  // hours
static uint8_t CLEANUP_ENABLED = 1;
static uint8_t SESSION_TIMEOUT = 2;  // seconds
static uint8_t MEASUREMENT_PRECISION = 10;  // meters

struct params {
    uint32_t *data;
    int length;
};

typedef struct pdsm_xtra_time_info {
    uint32_t uncertainty;
    uint64_t time_utc;
    bool_t ref_to_utc_time;
    bool_t force_flag;
} pdsm_xtra_time_info_type;

struct xtra_time_params {
    uint32_t *data;
    pdsm_xtra_time_info_type *time_info_ptr;
};

struct xtra_data_params {
    uint32_t *data;
    unsigned char *xtra_data_ptr;
    uint32_t part_len;
    uint8_t part;
    uint8_t total_parts;
};

struct xtra_validity_params {
    //Used in two functions pdsm_xtra_query_data_validity and pdsm_xtra_client_initiate_download_request
    uint32_t *data;
};

struct xtra_auto_params {
    uint32_t *data;
    uint8_t boolean;
    uint16_t interval;
};

static bool_t xdr_args(XDR *clnt, struct params *par) {
    int i;
    uint32_t val=0;
    for(i=0;par->length>i;++i)
        SEND_VAL(par->data[i]);
    return 1;
}

static bool_t xdr_result_int(XDR *clnt, uint32_t *result) {
    XDR_RECV_UINT32(clnt, result);
    return 1;
}

static bool_t xdr_xtra_data_args(XDR *xdrs, struct xtra_data_params *xtra_data) {
    //D("%s() is called: 0x%x, %d, %d, %d", __FUNCTION__, (int) xtra_data->xtra_data_ptr, xtra_data->part_len, xtra_data->part, xtra_data->total_parts);

    if (!xdr_u_long(xdrs, &xtra_data->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_data->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_data->data[2]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_data->part_len))
        return 0;
    if (!xdr_bytes(xdrs, (char **)&xtra_data->xtra_data_ptr, (u_int *)&xtra_data->part_len, ~0))
        return 0;
    if (!xdr_u_char(xdrs, &xtra_data->part))
        return 0;
    if (!xdr_u_char(xdrs, &xtra_data->total_parts))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_data->data[3]))
        return 0;

    return 1;
}

bool_t xdr_pdsm_xtra_time_info(XDR *xdrs, pdsm_xtra_time_info_type *time_info_ptr) {
    //D("%s() is called: %lld, %d", __FUNCTION__, time_info_ptr->time_utc, time_info_ptr->uncertainty);

    if (!xdr_u_quad_t(xdrs, &time_info_ptr->time_utc))
        return 0;
    if (!xdr_u_long(xdrs, &time_info_ptr->uncertainty))
        return 0;
    if (!xdr_u_char(xdrs, &time_info_ptr->ref_to_utc_time))
        return 0;
    if (!xdr_u_char(xdrs, &time_info_ptr->force_flag))
        return 0;

    return 1;
}

static bool_t xdr_xtra_time_args(XDR *xdrs, struct xtra_time_params *xtra_time) {
    //D("%s() is called", __FUNCTION__);

    if (!xdr_u_long(xdrs, &xtra_time->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_time->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_time->data[2]))
        return 0;
    if (!xdr_pointer(xdrs, (char **)&xtra_time->time_info_ptr, sizeof(pdsm_xtra_time_info_type), (xdrproc_t) xdr_pdsm_xtra_time_info))
        return 0;

    return 1;
}

static bool_t xdr_xtra_validity_args(XDR *xdrs, struct xtra_validity_params *xtra_validity) {
    //Used in two functions pdsm_xtra_query_data_validity and pdsm_xtra_client_initiate_download_request

    if (!xdr_u_long(xdrs, &xtra_validity->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_validity->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_validity->data[2]))
        return 0;

    return 1;
}

static bool_t xdr_xtra_auto_args(XDR *xdrs, struct xtra_auto_params *xtra_auto) {

    if (!xdr_u_long(xdrs, &xtra_auto->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_auto->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_auto->data[2]))
        return 0;
    if (!xdr_u_char(xdrs, &xtra_auto->boolean))
        return 0;
    if (!xdr_u_short(xdrs, &xtra_auto->interval))
        return 0;

    return 1;
}

static int pdsm_client_init(struct CLIENT *clnt, int client) {
    struct params par;
    uint32_t res;
    uint32_t par_data[1];
    par.data = par_data;
    par.length=1;
    par.data[0]=client;
    if(clnt_call(clnt, 0x2, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_init(%x) failed\n", client);
        exit(-1);
    }
    D("pdsm_client_init(%x)=%x\n", client, res);
    client_IDs[client]=res;
    return 0;
}

static int pdsm_client_release(struct CLIENT *clnt, int client) {
    struct params par;
    uint32_t res;
    uint32_t par_data;
    par.data = &par_data;
    par.length=1;
    par.data[0]=client_IDs[client];
    if(clnt_call(clnt, 0x3, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_release(%x) failed\n", client_IDs[client]);
        exit(-1);
    }
    D("pdsm_client_release(%x)=%x\n", client_IDs[client], res);
    client_IDs[client]=res;
    return 0;
}

int pdsm_atl_l2_proxy_reg(struct CLIENT *clnt, int val0, int val1, int val2) {
    struct params par;
    uint32_t res;
    uint32_t par_data[3];
    par.data = par_data;
    par.length=3;
    par.data[0]=val0;
    par.data[1]=val1;
    par.data[2]=val2;
    if(clnt_call(clnt, 0x3, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_atl_l2_proxy_reg(%d, %d, %d) failed\n", par.data[0], par.data[1], par.data[2]);
        exit(-1);
    }
    D("pdsm_atl_l2_proxy_reg(%d, %d, %d)=%d\n", par.data[0], par.data[1], par.data[2], res);
    return res;
}

int pdsm_atl_dns_proxy_reg(struct CLIENT *clnt, int val0, int val1) {
    struct params par;
    uint32_t res;
    uint32_t par_data[2];
    par.data = par_data;
    par.length=2;
    par.data[0]=val0;
    par.data[1]=val1;
    if(clnt_call(clnt, 0x6, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_atl_dns_proxy_reg(%d, %d) failed\n", par.data[0], par.data[1]);
        exit(-1);
    }
    D("pdsm_atl_dns_proxy(%d, %d)=%d\n", par.data[0], par.data[1], res);
    return res;
}

int pdsm_client_pd_reg(struct CLIENT *clnt, int client, int val0, int val1, int val2, int val3, int val4) {
    struct params par;
    uint32_t res;
    uint32_t par_data[6];
    par.data = par_data;
    par.length=6;
    par.data[0]=client_IDs[client];
    par.data[1]=val0;
    par.data[2]=val1;
    par.data[3]=val2;
    par.data[4]=val3;
    par.data[5]=val4;
    if(clnt_call(clnt, 0x4, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_pd_reg(%x, %d, %d, %d, %x, %d) failed\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5]);
        exit(-1);
    }
    D("pdsm_client_pd_reg(%x, %d, %d, %d, %x, %d)=%d\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5], res);
    return res;
}

int pdsm_client_pa_reg(struct CLIENT *clnt, int client, int val0, int val1, int val2, int val3, int val4) {
    struct params par;
    uint32_t res;
    uint32_t par_data[6];
    par.data = par_data;
    par.length=6;
    par.data[0]=client_IDs[client];
    par.data[1]=val0;
    par.data[2]=val1;
    par.data[3]=val2;
    par.data[4]=val3;
    par.data[5]=val4;
    if(clnt_call(clnt, 0x5, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_pa_reg(%x, %d, %d, %d, %x, %d) failed\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5]);
        exit(-1);
    }
    D("pdsm_client_pa_reg(%x, %d, %d, %d, %x, %d)=%d\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5], res);
    return res;
}

int pdsm_client_lcs_reg(struct CLIENT *clnt, int client, int val0, int val1, int val2, int val3, int val4) {
    struct params par;
    uint32_t res;
    uint32_t par_data[6];
    par.data = par_data;
    par.length=6;
    par.data[0]=client_IDs[client];
    par.data[1]=val0;
    par.data[2]=val1;
    par.data[3]=val2;
    par.data[4]=val3;
    par.data[5]=val4;
    if(clnt_call(clnt, 0x6, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_lcs_reg(%x, %d, %d, %d, %x, %d) failed\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5]);
        exit(-1);
    }
    D("pdsm_client_lcs_reg(%x, %d, %d, %d, %x, %d)=%d\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5], res);
    return res;
}

int pdsm_client_ext_status_reg(struct CLIENT *clnt, int client, int val0, int val1, int val2, int val3, int val4) {
    struct params par;
    uint32_t res;
    uint32_t par_data[6];
    par.data = par_data;
    par.length=6;
    par.data[0]=client_IDs[client];
    par.data[1]=val0;
    par.data[2]=val1;
    par.data[3]=val2;
    par.data[4]=val3;
    par.data[5]=val4;
    if(clnt_call(clnt, 0x8, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_ext_status_reg(%x, %d, %d, %d, %d, %d) failed\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5]);
        exit(-1);
    }
    D("pdsm_client_ext_status_reg(%x, %d, %d, %d, %d, %d)=%d\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5], res);
    return res;
}

int pdsm_client_xtra_reg(struct CLIENT *clnt, int client, int val0, int val1, int val2, int val3, int val4) {
    struct params par;
    uint32_t res;
    uint32_t par_data[6];
    par.data = par_data;
    par.length=6;
    par.data[0]=client_IDs[client];
    par.data[1]=val0;
    par.data[2]=val1;
    par.data[3]=val2;
    par.data[4]=val3;
    par.data[5]=val4;
    if(clnt_call(clnt, 0x7, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_xtra_reg(%x, %d, %d, %d, %d, %d) failed\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5]);
        exit(-1);
    }
    D("pdsm_client_xtra_reg(%x, %d, %d, %d, %d, %d)=%d\n", par.data[0], par.data[1], par.data[2], par.data[3], par.data[4], par.data[5], res);
    return res;
}

int pdsm_client_deact(struct CLIENT *clnt, int client) {
    struct params par;
    uint32_t res;
    uint32_t par_data;
    par.data = &par_data;
    par.length=1;
    par.data[0]=client_IDs[client];
    if(clnt_call(clnt, 0xA, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_deact(%x) failed\n", par.data[0]);
        exit(-1);
    }
    D("pdsm_client_deact(%x)=%d\n", par.data[0], res);
    return res;
}

int pdsm_client_act(struct CLIENT *clnt, int client) {
    struct params par;
    uint32_t res;
    uint32_t par_data[1];
    par.data = par_data;
    par.length=1;
    par.data[0]=client_IDs[client];
    if(clnt_call(clnt, 0x9, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_act(%x) failed\n", par.data[0]);
        exit(-1);
    }
    D("pdsm_client_act(%x)=%d\n", par.data[0], res);
    return res;
}

int pdsm_xtra_set_data(struct CLIENT *clnt, int val0, int client_ID, int val2, unsigned char *xtra_data_ptr, uint32_t part_len, uint8_t part, uint8_t total_parts, int val3) {
    struct xtra_data_params xtra_data;
    uint32_t res = -1;
    uint32_t par_data[4];
    xtra_data.data = par_data;
    xtra_data.data[0]=val0;
    xtra_data.data[1]=client_ID;
    xtra_data.data[2]=val2;
    xtra_data.xtra_data_ptr = xtra_data_ptr;
    xtra_data.part_len      = part_len;
    xtra_data.part          = part;
    xtra_data.total_parts   = total_parts;
    xtra_data.data[3]=val3;
    enum clnt_stat cs = -1;
    cs = clnt_call(clnt, 0x1A,
            (xdrproc_t) xdr_xtra_data_args,
            (caddr_t) &xtra_data,
            (xdrproc_t) xdr_result_int,
            (caddr_t) &res, timeout);
    //D("%s() is called: clnt_stat=%d", __FUNCTION__, cs);
    if (cs != RPC_SUCCESS){
        D("pdsm_xtra_set_data(%x, %x, %d, 0x%x, %d, %d, %d, %d) failed\n", val0, client_ID, val2, (int) xtra_data_ptr, part_len, part, total_parts, val3);
        exit(-1);
    }
    D("pdsm_xtra_set_data(%x, %x, %d, 0x%x, %d, %d, %d, %d)=%d\n", val0, client_ID, val2, (int) xtra_data_ptr, part_len, part, total_parts, val3, res);
    return res;
}

int pdsm_xtra_inject_time_info(struct CLIENT *clnt, int val0, int client_ID, int val2, pdsm_xtra_time_info_type *time_info_ptr) {
    struct xtra_time_params xtra_time;
    uint32_t res = -1;
    uint32_t par_data[3];
    xtra_time.data = par_data;
    xtra_time.data[0]=val0;
    xtra_time.data[1]=client_ID;
    xtra_time.data[2]=val2;
    xtra_time.time_info_ptr = time_info_ptr;
    enum clnt_stat cs = -1;
    cs = clnt_call(clnt, 0x1E,
            (xdrproc_t) xdr_xtra_time_args,
            (caddr_t) &xtra_time,
            (xdrproc_t) xdr_result_int,
            (caddr_t) &res, timeout);
    //D("%s() is called: clnt_stat=%d", __FUNCTION__, cs);
    if (cs != RPC_SUCCESS){
        D("pdsm_xtra_inject_time_info(%x, %x, %d, %lld, %d) failed\n", val0, client_ID, val2, time_info_ptr->time_utc, time_info_ptr->uncertainty);
        exit(-1);
    }
    D("pdsm_xtra_inject_time_info(%x, %x, %d, %lld, %d)=%d\n", val0, client_ID, val2, time_info_ptr->time_utc, time_info_ptr->uncertainty, res);
    return res;
}

int pdsm_xtra_query_data_validity(struct CLIENT *clnt, int val0, int client_ID, int val2) {
    //Not Tested Not Used
    struct xtra_validity_params xtra_validity;
    uint32_t res = -1;
    uint32_t par_data[3];
    xtra_validity.data = par_data;
    xtra_validity.data[0]=val0;
    xtra_validity.data[1]=client_ID;
    xtra_validity.data[2]=val2;
    enum clnt_stat cs = -1;
    cs = clnt_call(clnt, 0x1D,
            (xdrproc_t) xdr_xtra_validity_args,
            (caddr_t) &xtra_validity,
            (xdrproc_t) xdr_result_int,
            (caddr_t) &res, timeout);
    //D("%s() is called: clnt_stat=%d", __FUNCTION__, cs);
    if (cs != RPC_SUCCESS){
        D("pdsm_xtra_query_data_validity(%x, %x, %d) failed\n", val0, client_ID, val2);
        exit(-1);
    }
    D("pdsm_xtra_query_data_validity(%x, %x, %d)=%d\n", val0, client_ID, val2, res);
    return res;
}

int pdsm_xtra_set_auto_download_params(struct CLIENT *clnt, int val0, int client_ID, int val2, uint8_t boolean, uint16_t interval) {
    struct xtra_auto_params xtra_auto;
    uint32_t res = -1;
    uint32_t par_data[3];
    xtra_auto.data = par_data;
    xtra_auto.data[0]=val0;
    xtra_auto.data[1]=client_ID;
    xtra_auto.data[2]=val2;
    xtra_auto.boolean=boolean;
    xtra_auto.interval=interval;
    enum clnt_stat cs = -1;
    cs = clnt_call(clnt, 0x1C,
            (xdrproc_t) xdr_xtra_auto_args,
            (caddr_t) &xtra_auto,
            (xdrproc_t) xdr_result_int,
            (caddr_t) &res, timeout);
    //D("%s() is called: clnt_stat=%d", __FUNCTION__, cs);
    if (cs != RPC_SUCCESS){
        D("pdsm_xtra_set_auto_download_params(%x, %x, %d, %d, %d) failed\n", val0, client_ID, val2, boolean, interval);
        exit(-1);
    }
    D("pdsm_xtra_set_auto_download_params(%x, %x, %d, %d, %d)=%d\n", val0, client_ID, val2, boolean, interval, res);
    return res;
}

int pdsm_xtra_client_initiate_download_request(struct CLIENT *clnt, int val0, int client_ID, int val2) {
    //Works but not currently being used
    struct xtra_validity_params xtra_request;
    uint32_t res = -1;
    uint32_t par_data[3];
    xtra_request.data = par_data;
    xtra_request.data[0]=val0;
    xtra_request.data[1]=client_ID;
    xtra_request.data[2]=val2;
    enum clnt_stat cs = -1;
    cs = clnt_call(clnt, 0x1B,
            (xdrproc_t) xdr_xtra_validity_args,
            (caddr_t) &xtra_request,
            (xdrproc_t) xdr_result_int,
            (caddr_t) &res, timeout);
    //D("%s() is called: clnt_stat=%d", __FUNCTION__, cs);
    if (cs != RPC_SUCCESS){
        D("pdsm_xtra_client_initiate_download_request(%x, %x, %d) failed\n", val0, client_ID, val2);
        exit(-1);
    }
    D("pdsm_xtra_client_initiate_download_request(%x, %x, %d)=%d\n", val0, client_ID, val2, res);
    return res;
}

int pdsm_get_position(struct CLIENT *clnt, int val0, int val1, int val2, int val3, int val4, int val5, int val6, int val7, int val8, int val9, int val10, int val11, int val12, int val13, int val14, int val15, int val16, int 
val17, int val18, int val19, int val20, int val21, int val22, int val23, int val24, int val25, int val26, int val27, int val28) 
{
    struct params par;
    uint32_t res;
    uint32_t par_data[29];
    par.data = par_data;
    par.length=29;
    par.data[0]=val0;
    par.data[1]=val1;
    par.data[2]=val2;
    par.data[3]=val3;
    par.data[4]=val4;
    par.data[5]=val5;
    par.data[6]=val6;
    par.data[7]=val7;
    par.data[8]=val8;
    par.data[9]=val9;
    par.data[10]=val10;
    par.data[11]=val11;
    par.data[12]=val12;
    par.data[13]=val13;
    par.data[14]=val14;
    par.data[15]=val15;
    par.data[16]=val16;
    par.data[17]=val17;
    par.data[18]=val18;
    par.data[19]=val19;
    par.data[20]=val20;
    par.data[21]=val21;
    par.data[22]=val22;
    par.data[23]=val23;
    par.data[24]=val24;
    par.data[25]=val25;
    par.data[26]=val26;
    par.data[27]=val27;
    par.data[28]=val28;
    if(clnt_call(clnt, 0xb, 
             (xdrproc_t)xdr_args, 
             (caddr_t)&par, 
             (xdrproc_t)xdr_result_int, 
             (caddr_t)&res, timeout)) 
    {
        D("pdsm_client_get_position() failed\n");
        exit(-1);
    }
    D("pdsm_client_get_position()=%d\n", res);
    return res;
}

int pdsm_client_end_session(struct CLIENT *clnt, int val0, int val1, int val2, int client) {
    struct params par;
    uint32_t res;
    uint32_t par_data[4];
    par.data = par_data;
    par.length=4;
    par.data[0]=val0;
    par.data[1]=val1;
    par.data[2]=val2;
    par.data[3]=client_IDs[client];
    if(clnt_call(clnt, 0xc, xdr_args, &par, xdr_result_int, &res, timeout)) {
        D("pdsm_client_end_session(%d, %d, %d, %x) failed\n", par.data[0], par.data[1], par.data[2], par.data[3]);
        exit(-1);
    }
    D("pdsm_client_end_session(%d, %d, %d, %x)=%x\n", par.data[0], par.data[1], par.data[2], par.data[3], res);
    return 0;
}

enum pdsm_pd_events {
    PDSM_PD_EVENT_POSITION = 0x1,
    PDSM_PD_EVENT_VELOCITY = 0x2,
    PDSM_PD_EVENT_HEIGHT = 0x4,
    PDSM_PD_EVENT_DONE = 0x8,
    PDSM_PD_EVENT_END = 0x10,
    PDSM_PD_EVENT_BEGIN = 0x20,
    PDSM_PD_EVENT_COMM_BEGIN = 0x40,
    PDSM_PD_EVENT_COMM_CONNECTED = 0x80,
    PDSM_PD_EVENT_COMM_DONE = 0x200,
    PDSM_PD_EVENT_GPS_BEGIN = 0x4000,
    PDSM_PD_EVENT_GPS_DONE = 0x8000,
    PDSM_PD_EVENT_UPDATE_FAIL = 0x1000000,
};

//From leo-gps.c
extern void update_gps_location(GpsLocation *location);
extern void update_gps_status(GpsStatusValue value);
extern void update_gps_svstatus(GpsSvStatus *svstatus);

void dispatch_pdsm_pd(uint32_t *data) {
    uint32_t event=ntohl(data[2]);
    D("%s(): event=0x%x", __FUNCTION__, event);
    if(event&PDSM_PD_EVENT_BEGIN) {
        D("PDSM_PD_EVENT_BEGIN");
    }
    if(event&PDSM_PD_EVENT_GPS_BEGIN) {
        D("PDSM_PD_EVENT_GPS_BEGIN");
    }
    if(event&PDSM_PD_EVENT_GPS_DONE) {
        D("PDSM_PD_EVENT_GPS_DONE");
        no_fix = 1;
    }
    GpsLocation fix;
    fix.flags = 0;
    if(event&PDSM_PD_EVENT_POSITION) {
        D("PDSM_PD_EVENT_POSITION");
        if (use_nmea) return;

        GpsSvStatus ret;
        int i;
        ret.num_svs=ntohl(data[82]) & 0x1F;

#if DUMP_DATA
        //D("pd %3d: %08x ", 77, ntohl(data[77]));
        for(i=60;i<83;++i) {
            D("pd %3d: %08x ", i, ntohl(data[i]));
        }
        for(i=83;i<83+3*(ret.num_svs-1)+3;++i) {
            D("pd %3d: %d ", i, ntohl(data[i]));
        }
#endif

        for(i=0;i<ret.num_svs;++i) {
            ret.sv_list[i].prn=ntohl(data[83+3*i]);
            ret.sv_list[i].elevation=ntohl(data[83+3*i+1]);
            ret.sv_list[i].azimuth=(float)ntohl(data[83+3*i+2])/100.0f;
            ret.sv_list[i].snr=ntohl(data[83+3*i+2])%100;
        }
        ret.used_in_fix_mask=ntohl(data[77]);
        update_gps_svstatus(&ret);

        fix.timestamp = ntohl(data[8]);
        if (!fix.timestamp) return;

        // convert gps time to epoch time ms
        fix.timestamp += 315964800; // 1/1/1970 to 1/6/1980
        fix.timestamp -= 15; // 15 leap seconds between 1980 and 2011
        fix.timestamp *= 1000; //ms

        fix.flags |= GPS_LOCATION_HAS_LAT_LONG;
        no_fix = 0;

        if (ntohl(data[75])) {
            fix.flags |= GPS_LOCATION_HAS_ACCURACY;
            float hdop = (float)ntohl(data[75]) / 10.0f / 2.0f;
            fix.accuracy = hdop * (float)MEASUREMENT_PRECISION;
        }

        union {
            struct {
                uint32_t lowPart;
                int32_t highPart;
            };
            int64_t int64Part;
        } latitude, longitude;

        latitude.lowPart = ntohl(data[61]);
        latitude.highPart = ntohl(data[60]);
        longitude.lowPart = ntohl(data[63]);
        longitude.highPart = ntohl(data[62]);
        fix.latitude = (double)latitude.int64Part / 1.0E8;
        fix.longitude = (double)longitude.int64Part / 1.0E8;
    }
    if (event&PDSM_PD_EVENT_VELOCITY)
    {
        D("PDSM_PD_EVENT_VELOCITY");
        if (use_nmea) return;
        fix.flags |= GPS_LOCATION_HAS_SPEED|GPS_LOCATION_HAS_BEARING;
        fix.speed = (float)ntohl(data[66]) / 10.0f / 3.6f; // convert kp/h to m/s
        fix.bearing = (float)ntohl(data[67]) / 10.0f;
    }
    if (event&PDSM_PD_EVENT_HEIGHT)
    {
        D("PDSM_PD_EVENT_HEIGHT");
        if (use_nmea) return;
        fix.flags |= GPS_LOCATION_HAS_ALTITUDE;
        fix.altitude = 0;
        double altitude = (double)ntohl(data[64]);
        if (altitude / 10.0f < 1000000.0) // Check if height is not unreasonably high
            fix.altitude = altitude / 10.0f; // Apply height with a division of 10 to correct unit of meters
        else // If unreasonably high then it is a negative height
            fix.altitude = (altitude - (double)4294967295.0) / 10.0f; // Subtract FFFFFFFF to make height negative
    }
    if (fix.flags)
    {
        update_gps_location(&fix);
    }
    if(event&PDSM_PD_EVENT_END)
    {
        D("PDSM_PD_EVENT_END");
    }
    if(event&PDSM_PD_EVENT_DONE)
    {
        D("PDSM_PD_EVENT_DONE");
        pdsm_pd_callback();
    }
}

void dispatch_pdsm_ext(uint32_t *data) {
    GpsSvStatus ret;
    int i;

    if (use_nmea) return;

    no_fix++;
    if (no_fix < 2) return;
    
    ret.num_svs=ntohl(data[8]);
    D("%s() is called. num_svs=%d", __FUNCTION__, ret.num_svs);

#if DUMP_DATA
    for(i=0;i<12;++i) {
        D("e %3d: %08x ", i, ntohl(data[i]));
    }
    for(i=101;i<101+12*(ret.num_svs-1)+6;++i) {
        D("e %3d: %d ", i, ntohl(data[i]));
    }
#endif

    for(i=0;i<ret.num_svs;++i) {
        ret.sv_list[i].prn=ntohl(data[101+12*i+1]);
        ret.sv_list[i].elevation=ntohl(data[101+12*i+5]);
        ret.sv_list[i].azimuth=ntohl(data[101+12*i+4]);
        ret.sv_list[i].snr=(float)ntohl(data[101+12*i+2])/10.0f;
    }
    //ret.used_in_fix_mask=ntohl(data[9]);
    ret.used_in_fix_mask=0;
    update_gps_svstatus(&ret);
}

void dispatch_pdsm_xtra_req(uint8_t *data) {
    //Handles download requests from gps chip
    //Have to check if it is a download request because the same procid is multipurpose
    
    unsigned char url[9]; //Stores the filename for the xtra data
    unsigned int i = 0x50;
    
    memcpy(url, &(data[i]), 8); //Copies the filename from the rpc message
    url[8] = '\0'; //Adds the null terminate at the end of the filename to create a string
    
    // Performs comparison with expected string "xtra.bin"
    if (strcmp(url, "xtra.bin") == 0) {
        D("Calling xtra_download_request()");
        //Calls the gps_xtra_download_request callback method
        xtra_download_request();
    }
}

void dispatch_pdsm(uint32_t *data) {
    uint32_t procid=ntohl(data[5]);
    D("%s() is called. data[5]=procid=%d", __FUNCTION__, procid);
    if(procid==1) 
        dispatch_pdsm_pd(&(data[10]));
    else if(procid==4) 
        dispatch_pdsm_ext(&(data[10]));
    else if(procid==5)
        dispatch_pdsm_xtra_req(&(data[10]));
}

void dispatch_atl(uint32_t *data) {
    D("%s() is called", __FUNCTION__);
    // No clue what happens here.
}

void dispatch(struct svc_req* a, registered_server* svc) {
    int i;
    uint32_t *data=svc->xdr->in_msg;
    uint32_t result=0;
    uint32_t svid=ntohl(data[3]);
/*
    D("received some kind of event\n");
    for(i=0;i< svc->xdr->in_len/4;++i) {
        D("%08x ", ntohl(data[i]));
    }
    D("\n");
    for(i=0;i< svc->xdr->in_len/4;++i) {
        D("%010d ", ntohl(data[i]));
    }
    D("\n");
*/
    if(svid==0x3100005b) {
        dispatch_pdsm(data);
    } else if(svid==0x3100001d) {
        dispatch_atl(data);
    } else {
        //Got dispatch for unknown serv id!
    }
    //ACK
    svc_sendreply(svc, xdr_int, &result);
}

uint8_t get_cleanup_value() {
    D("%s() is called: %d", __FUNCTION__, CLEANUP_ENABLED);
    return CLEANUP_ENABLED;
}

uint8_t get_precision_value() {
    D("%s() is called: %d", __FUNCTION__, MEASUREMENT_PRECISION);
    return MEASUREMENT_PRECISION;
}

int parse_gps_conf() {
    FILE *file = fopen("/system/etc/gps.conf", "r");
    if (!file) { 
        D("fopen error\n");
        return 1; 
    }
    
    char *check_auto_download = "GPS1_XTRA_AUTO_DOWNLOAD_ENABLED";
    char *check_interval = "GPS1_XTRA_DOWNLOAD_INTERVAL";
    char *check_cleanup = "GPS1_CLEANUP_ENABLED";
    char *check_timeout = "GPS1_SESSION_TIMEOUT";
    char *check_precision = "GPS1_MEASUREMENT_PRECISION";
    char *result;
    char str[256];
    int i = -1;

    while (fscanf(file, "%s", str) != EOF) {
        //D("%s (%d)\n", str, strlen(str));
        if (!CHECKED[1]) {
            result = strstr(str, check_auto_download);
            if (result != NULL) {
                result = result+strlen(check_auto_download)+1;
                i = atoi(result);
                if (i==0 || i==1)
                    XTRA_AUTO_DOWNLOAD_ENABLED = i;
                CHECKED[1] = 1;
            }
        }
        if (XTRA_AUTO_DOWNLOAD_ENABLED) {
            result = strstr(str, check_interval);
            if (result != NULL) {
                result = result+strlen(check_interval)+1;
                i = atoi(result);
                if (i>0 && i<169)
                    XTRA_DOWNLOAD_INTERVAL = i;
            }
        }
        if (!CHECKED[2]) {
            result = strstr(str, check_cleanup);
            if (result != NULL) {
                result = result+strlen(check_cleanup)+1;
                i = atoi(result);
                if (i==0 || i==1)
                    CLEANUP_ENABLED = i;
                CHECKED[2] = 1;
            }
        }
        if (!CHECKED[3]) {
            result = strstr(str, check_timeout);
            if (result != NULL) {
                result = result+strlen(check_timeout)+1;
                i = atoi(result);
                if (i>1 && i<121)
                    SESSION_TIMEOUT = i;
                CHECKED[3] = 1;
            }
        }
        if (!CHECKED[4]) {
            result = strstr(str, check_precision);
            if (result != NULL) {
                result = result+strlen(check_precision)+1;
                i = atoi(result);
                if (i>0 && i<16)
                    MEASUREMENT_PRECISION = i;
                CHECKED[4] = 1;
            }
        }
    }
    fclose(file);
    LOGD("%s() is called: GPS1_XTRA_AUTO_DOWNLOAD_ENABLED = %d", __FUNCTION__, XTRA_AUTO_DOWNLOAD_ENABLED);
    LOGD("%s() is called: GPS1_XTRA_DOWNLOAD_INTERVAL = %d", __FUNCTION__, XTRA_DOWNLOAD_INTERVAL);
    LOGD("%s() is called: GPS1_CLEANUP_ENABLED = %d", __FUNCTION__, CLEANUP_ENABLED);
    LOGD("%s() is called: GPS1_SESSION_TIMEOUT = %d", __FUNCTION__, SESSION_TIMEOUT);
    LOGD("%s() is called: GPS1_MEASUREMENT_PRECISION = %d", __FUNCTION__, MEASUREMENT_PRECISION);
    return 0;
}

int init_leo() 
{
    struct CLIENT *clnt=clnt_create(NULL, 0x3000005B, 0x00010001, NULL);
    struct CLIENT *clnt_atl=clnt_create(NULL, 0x3000001D, 0x00010001, NULL);
    int i;
    _clnt=clnt;
    SVCXPRT *svc=svcrtr_create();
    _svc=svc;
    xprt_register(svc);
    svc_register(svc, 0x3100005b, 0x00010001, (__dispatch_fn_t)dispatch, 0);
    svc_register(svc, 0x3100005b, 0, (__dispatch_fn_t)dispatch, 0);
    svc_register(svc, 0x3100001d, 0x00010001, (__dispatch_fn_t)dispatch, 0);
    svc_register(svc, 0x3100001d, 0, (__dispatch_fn_t)dispatch, 0);
    if(!clnt) {
        D("Failed creating client\n");
        return -1;
    }
    if(!svc) {
        D("Failed creating server\n");
        return -2;
    }

    // PDA
    pdsm_client_init(clnt, 2);
    pdsm_client_pd_reg(clnt, 2, 0, 0, 0, 0xF3F0FFFF, 0);
    pdsm_client_pa_reg(clnt, 2, 0, 2, 0, 0x7FFEFE0, 0);
    pdsm_client_ext_status_reg(clnt, 2, 0, 1, 0, 4, 0);
    pdsm_client_act(clnt, 2);

    // XTRA
    pdsm_client_init(clnt, 0xb);
    pdsm_client_xtra_reg(clnt, 0xb, 0, 3, 0, 7, 0);
    pdsm_client_act(clnt, 0xb);
    pdsm_atl_l2_proxy_reg(clnt_atl, 1,0,0);
    pdsm_atl_dns_proxy_reg(clnt_atl, 1,0);

    // NI
    pdsm_client_init(clnt, 4);
    pdsm_client_lcs_reg(clnt, 4, 0, 7, 0, 0x3F0, 0);
    pdsm_client_act(clnt, 4);
    
    if (!CHECKED[0]) {
        if (use_nmea)
            LOGD("%s() is called: %s version", __FUNCTION__, "NMEA");
        else
            LOGD("%s() is called: %s version", __FUNCTION__, "RPC");

        parse_gps_conf();
        if (XTRA_AUTO_DOWNLOAD_ENABLED)
            gps_xtra_set_auto_params();
        CHECKED[0] = 1;
    }

    return 0;
}

int init_gps_rpc() 
{
    init_leo();
    return 0;
}

int gps_xtra_set_data(unsigned char *xtra_data_ptr, uint32_t part_len, uint8_t part, uint8_t total_parts) 
{
    uint32_t res = -1;
    res = pdsm_xtra_set_data(_clnt, 0, client_IDs[0xb], 0, xtra_data_ptr, part_len, part, total_parts, 1);
    return res;
}

int gps_xtra_init_down_req() 
{
    //Tell gpsOne to request xtra data
    uint32_t res = -1;
    res = pdsm_xtra_client_initiate_download_request(_clnt, 0, client_IDs[0xb], 0);
    return res;
}

int gps_xtra_set_auto_params() 
{
    //Set xtra auto download parameters
    uint32_t res = -1;
    uint8_t boolean = XTRA_AUTO_DOWNLOAD_ENABLED; //Enable/Disable
    uint16_t interval = XTRA_DOWNLOAD_INTERVAL; //Interval in hours 1 to 168(Week)
    res = pdsm_xtra_set_auto_download_params(_clnt, 0, client_IDs[0xb], 0, boolean, interval);
    return res;
}

extern int64_t elapsed_realtime();

int gps_xtra_inject_time_info(GpsUtcTime time, int64_t timeReference, int uncertainty)
{
    uint32_t res = -1;
    pdsm_xtra_time_info_type time_info_ptr;
    time_info_ptr.uncertainty = uncertainty;
    time_info_ptr.time_utc = time;
    time_info_ptr.time_utc += (int64_t)(elapsed_realtime() - timeReference);
    time_info_ptr.ref_to_utc_time = 1;
    time_info_ptr.force_flag = 1;
    res = pdsm_xtra_inject_time_info(_clnt, 0, client_IDs[0xb], 0, &time_info_ptr);
    return res;
}

void gps_get_position() 
{
#if GPS_DEBUG
    struct tm  tm;
    time_t  now = time(NULL);
    gmtime_r( &now, &tm );
    long time = mktime(&tm);
    D("%s() is called: %ld", __FUNCTION__, time);
#endif
    pdsm_get_position(_clnt, 
            0, 0,           
            1,              
            1, 1,           
            0x3B9AC9FF, 1,  
        0,                  
        0, 0,               
        0, 0,               
        0,                  
       0, 0, 0, 0, 0, 0, 0, 
       0, 0, 0, 0, 0,       
       1, 50, SESSION_TIMEOUT,
       client_IDs[2]);
}

void exit_gps_rpc() 
{
    pdsm_client_end_session(_clnt, 0, 0, 0, 2);
}

void cleanup_gps_rpc_clients() 
{
    pdsm_client_deact(_clnt, 2);
    pdsm_client_deact(_clnt, 0xb);
    pdsm_client_deact(_clnt, 4);
    
    pdsm_client_release(_clnt, 2);
    pdsm_client_release(_clnt, 0xb);
    pdsm_client_release(_clnt, 4);
    
    svc_unregister(_svc, 0x3100005b, 0x00010001);
    svc_unregister(_svc, 0x3100005b, 0);
    svc_unregister(_svc, 0x3100001d, 0x00010001);
    svc_unregister(_svc, 0x3100001d, 0);
    xprt_unregister(_svc);
    svc_destroy(_svc);
    
    clnt_destroy(_clnt);
}

// END OF FILE

/*
 * Copyright (C) Apcera Inc.
 *
 * Based on Nginx source code:
 *              Copyright (C) Igor Sysoev
 *              Copyright (C) Nginx, Inc.
 */

#ifndef _NGX_NATS_COMM_H_INCLUDED_
#define _NGX_NATS_COMM_H_INCLUDED_

#include "ngx_nats.h"


#define NGX_NATS_REASON_CONNECT_FAILED  (1) /* unable to socket into NATS   */
#define NGX_NATS_REASON_CONNECT_REFUSED (2) /* NATS refused connect (auth)  */
#define NGX_NATS_REASON_READ_TIMEOUT    (3) /* reading from NATS timed out  */
#define NGX_NATS_REASON_WRITE_TIMEOUT   (4) /* writing into NATS timed out  */
#define NGX_NATS_REASON_BAD_PROTOCOL    (5) /* NATS sent msg I cant parse   */
#define NGX_NATS_REASON_DISCONNECTED    (6) /* NATS disconnected            */
#define NGX_NATS_REASON_SHUTTING_DOWN   (7) /* nginx process is shutting down */
#define NGX_NATS_REASON_NO_MEMORY       (98)
#define NGX_NATS_REASON_INTERNAL_ERROR  (99)


#define NGX_NATS_READ_BUF_INIT_SIZE     (4096)
#define NGX_NATS_WRITE_BUF_INIT_SIZE    (4096)


#define NGX_NATS_STATE_BYTES_EXCHANGED  0x01
#define NGX_NATS_STATE_CONNECT_SENT     0x02
#define NGX_NATS_STATE_CONNECT_OKAYED   0x04
#define NGX_NATS_STATE_INFO_RECEIVED    0x08
#define NGX_NATS_STATE_READY            0x0f


#define NGX_NATS_MAX_MESSAGE_SIZE       (256 * 1024 * 1024)     /* 256MB */

typedef struct ngx_nats_data_s ngx_nats_data_t;


typedef struct {
    ngx_log_t      *log;

    /*
     * The readable/sendable portion of the buffer is between
     * pos and end, 0<=pos<=end<=cap. "cap" is allocated length.
     */
    u_char         *buf;
    size_t          pos;
    size_t          end;
    size_t          cap;

} ngx_nats_buf_t;


typedef struct {

    ngx_pool_t             *pool;

    ngx_nats_data_t        *nd;
    ngx_nats_server_t      *server;     /* from ngx_nats_core_conf_t    */

    ngx_peer_connection_t   pc;
    ngx_event_t             ping_timer;

    ngx_nats_buf_t         *read_buf;
    ngx_nats_buf_t         *write_buf;

    /* Sent by NATS in INFO message */
    ngx_str_t              *srv_id;
    ngx_str_t              *srv_host;
    ngx_int_t               srv_port;
    ngx_str_t              *srv_version;
    ngx_str_t              *go_version;
    ngx_int_t               srv_max_payload;
    unsigned                srv_auth_required:1;
    unsigned                srv_ssl_required:1;

    /* Stages/state of connecting to NATS */
    u_char                  state;

} ngx_nats_connection_t;


typedef struct {

    ngx_nats_client_t      *client;
    ngx_nats_handle_msg_pt  handle_msg;
    void                   *client_subscription_data;
    ngx_int_t               sid;
    ngx_int_t               max;
    ngx_int_t               recv;

} ngx_nats_subscription_t;


typedef struct {

    ngx_array_t             clients;        /* ngx_nats_client_t*           */

    /*
     * TODO: use ngx_hash? array more economical if just a few
     * subscriptions.
     */
    ngx_array_t             subs;           /* ngx_nats_subscription_t      */
    ngx_int_t               next_id;

} ngx_nats_client_data_t;


struct ngx_nats_data_s {

    ngx_nats_core_conf_t   *nccf;
    ngx_log_t              *log;

    ngx_event_t             reconnect_timer;
    ngx_int_t               nconnects;      /* unsuccessful connects        */

    ngx_nats_client_data_t  cd;

    ngx_nats_connection_t  *nc;             /* currently one only           */
    ngx_pool_t             *nc_pool;
    ngx_nats_buf_t         *nc_read_buf;
    ngx_nats_buf_t         *nc_write_buf;

    ngx_int_t               conn_index;     /* connected in nccf->servers   */
    ngx_int_t               curr_index;     /* for loop-through connect     */
    ngx_int_t               last_index;     /* last connected to            */

};


/*
 * The entire buf story needs improvement, especially write buffers.
 * Current impl will do for low rate of sends to NATS. If NATS is used
 * to process HTTP requests or in similar high-performance fashion
 * then this has to change, but no plans for now.
 */
ngx_nats_buf_t * ngx_nats_buf_create(ngx_pool_t *pool, size_t size);
void ngx_nats_buf_free_buf(ngx_nats_buf_t *buf);
ngx_int_t ngx_nats_buf_ensure(ngx_nats_buf_t *buf, size_t size,
                ngx_int_t compact);
void ngx_nats_buf_reset(ngx_nats_buf_t *buf);
void ngx_nats_buf_compact(ngx_nats_buf_t *buf);


#endif /* _NGX_NATS_COMM_H_INCLUDED_ */


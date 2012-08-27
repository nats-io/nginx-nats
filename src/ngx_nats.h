/*
 * Copyright (C) Apcera Inc.
 *
 * Based on Nginx source code:
 *              Copyright (C) Igor Sysoev
 *              Copyright (C) Nginx, Inc.
 */

#ifndef _NGX_NATS_H_INCLUDED_
#define _NGX_NATS_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>

/*
 * Nginx reverses the characters so printing this directly on
 * little endian processors will print it correctly. This is
 * stored in an int variable.
 */
#define NGX_NATS_MODULE             0x5354414E      /* "NATS" in reverse */

#define NGX_NATS_CONF               0x02000000

#define NGX_NATS_CONN_POOL_SIZE     (1024)

#define NGX_NATS_DEFAULT_PORT       4222
#define NGX_NATS_DEFAULT_RECONNECT  1000            /* milliseconds */
#define NGX_NATS_DEFAULT_PING       20000           /* milliseconds */

#define NGX_NATS_DEFAULT_USER               "continuum.router"

#define NGX_NATS_MAS_USER_PASS_LEN  (512)


/*
 * Used by ngx_nats_core_module and maybe other future NATS modules.
 * ngx_nats_module is not of this type, it is of type ngx_core_module_t.
 */
typedef struct {
    ngx_str_t               name;
    void                   *(*create_conf)(ngx_cycle_t *cycle);
    char                   *(*init_conf)(ngx_conf_t *cf, void *conf);

} ngx_nats_module_t;


/*
 * NATS server description from the nats{...} config.
 */
typedef struct {
    ngx_str_t               url;

    ngx_addr_t             *addrs;
    ngx_uint_t              naddrs;

} ngx_nats_server_t;

/*
 * Core configuration of NATS.
 */
typedef struct {
    ngx_str_t              *name;       /* module name                      */

    ngx_pool_t             *pool;       /* from cycle->pool                 */
    ngx_log_t              *log;        /* TODO: dedicated NATS log?        */

    void                   *data;       /* pointer to ngx_nats_data_t       */

    /* From configuration or defaults */

    ngx_array_t            *servers;    /* of ngx_nats_server_t elements    */
    
    ngx_msec_t              reconnect_interval;     /* millis */
    ngx_msec_t              ping_interval;          /* millis */

    ngx_str_t               user;       /* common in nats{...} section      */
    ngx_str_t               password;

} ngx_nats_core_conf_t;


#define ngx_nats_get_conf(ctx, module)                                  \
            (*(ngx_get_conf(ctx, ngx_nats_module))) [module.ctx_index];
#define ngx_nats_get_core_conf(ctx)                                  \
            (*(ngx_get_conf(ctx, ngx_nats_module))) [ngx_nats_core_module.ctx_index];


/*
 * Main initialization called from process init function.
 * Implemented in ngx_nats_comm.c.
 */
ngx_int_t ngx_nats_init(ngx_nats_core_conf_t * nccf);
void ngx_nats_exit(ngx_nats_core_conf_t * nccf);


/*
 * Public client interface.
 */

typedef struct ngx_nats_client_s ngx_nats_client_t;

typedef void (*ngx_nats_connected_pt)(ngx_nats_client_t *client);
typedef void (*ngx_nats_disconnected_pt)(ngx_nats_client_t *client);
typedef void (*ngx_nats_handle_msg_pt)(ngx_nats_client_t *client,
                    ngx_str_t *subject, ngx_int_t sid, ngx_str_t *replyto,
                    u_char *data, size_t len);

struct ngx_nats_client_s {

    ngx_nats_connected_pt       connected;
    ngx_nats_disconnected_pt    disconnected;

    void                       *data;

};

ngx_int_t ngx_nats_add_client(ngx_nats_client_t *client);
ngx_int_t ngx_nats_publish(ngx_nats_client_t *client, ngx_str_t *subject,
                ngx_str_t *replyto, u_char *data, ngx_uint_t len);
ngx_int_t ngx_nats_subscribe(ngx_nats_client_t *client, ngx_str_t *subject,
                ngx_nats_handle_msg_pt handle_msg);
ngx_int_t ngx_nats_create_inbox(u_char *buf, size_t bufsize);


#endif /* _NGX_NATS_H_INCLUDED_ */


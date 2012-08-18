/*
 * Copyright (C) Apcera, Inc.
 *
 * Based on Nginx source code:
 *              Copyright (C) Igor Sysoev
 *              Copyright (C) Nginx, Inc.
 */

#include "ngx_nats_comm.h"
#include "ngx_nats_protocol.h"
#include "ngx_nats_json.h"

/*
 * Implements communication with the NATS server. Connect, reconnect,
 * sending PINGs, sending messages and processing of the incoming messages.
 */

/*---------------------------------------------------------------------------
 * Forward declarations of functions.
 *--------------------------------------------------------------------------*/

static void ngx_nats_connect(ngx_nats_data_t *nd);
static void ngx_nats_process_reconnect(ngx_nats_data_t *nd);
static void ngx_nats_add_reconnect_timer(ngx_nats_data_t * nd);

static void ngx_nats_connection_init(ngx_nats_connection_t *nc);
static void ngx_nats_flush(ngx_nats_connection_t *nc);
static ngx_int_t ngx_nats_add_message(ngx_nats_connection_t *nc,
                    char *message, size_t size);

static ngx_int_t ngx_nats_get_peer(ngx_peer_connection_t *pc, void *data);
static void ngx_nats_free_peer(ngx_peer_connection_t *pc,
                    void *data, ngx_uint_t state);

static ngx_int_t ngx_nats_conn_err_reported = 0;

/*---------------------------------------------------------------------------
 * Implementations.
 *--------------------------------------------------------------------------*/

ngx_nats_buf_t *
ngx_nats_buf_create(ngx_pool_t *pool, size_t size)
{
    ngx_nats_buf_t *buf;

    buf = ngx_pcalloc(pool, sizeof(ngx_nats_buf_t));
    if (buf == NULL) {
        return NULL;
    }

    buf->log = pool->log;

    buf->buf = ngx_alloc(size, pool->log);
    if (buf->buf == NULL) {
        return NULL;
    }

    buf->cap = size;

    return buf;
}


void
ngx_nats_buf_free_buf(ngx_nats_buf_t *buf)
{
    if (buf->buf != NULL) {
        ngx_free(buf->buf);
    }

    ngx_memzero(buf, sizeof(ngx_nats_buf_t));
}


void
ngx_nats_buf_reset(ngx_nats_buf_t *buf)
{
    buf->pos = 0;
    buf->end = 0;
}


void
ngx_nats_buf_compact(ngx_nats_buf_t *buf)
{
    if (buf->pos == buf->end) {
        buf->pos = 0;
        buf->end = 0;
        return;
    }

    if (buf->pos > 0) {
        ngx_memmove(buf->buf, buf->buf + buf->pos, buf->end - buf->pos);
        buf->end -= buf->pos;
        buf->pos = 0;
    }
}


ngx_int_t
ngx_nats_buf_ensure(ngx_nats_buf_t *buf, size_t size, ngx_int_t compact)
{
    size_t  n, tail, ns, nt;
    char   *bnew;

    if (buf->pos == buf->end && buf->pos != 0) {
        buf->pos = 0;
        buf->end = 0;
    }

    tail = buf->cap - buf->end;
    if (tail >= size) {
        return NGX_OK;
    }

    if (compact != 0 && buf->pos > 0 && (size <= (tail + buf->pos))) {
        ngx_memmove(buf->buf, buf->buf + buf->pos, buf->end - buf->pos);
        buf->end -= buf->pos;
        buf->pos = 0;
        tail = buf->cap - buf->end;
    }

    if (tail >= size) {
        return NGX_OK;
    }

    if (buf->cap >= (256*1024*1024)) {
        ngx_log_error(NGX_LOG_CRIT, buf->log, 0,
        "attempt to increase NATS buffer size to more than 256MB");
        return NGX_ERROR;
    }

    /* realloc */

    n  = buf->end - buf->pos;       /* current bytes */
    nt = n + size;                  /* total need    */

    ns = buf->cap * 2;
    while(ns < nt) {
        ns <<= 1;       /* checked above cap is limited */
    }

    bnew = ngx_alloc(ns, buf->log);
    if (bnew == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(bnew, buf->buf + buf->pos, n);

    ngx_free(buf->buf);
    buf->buf = bnew;
    buf->cap = ns;

    return NGX_OK;
}


/*
 * Copied from ngx_http_upstream.c.
 */
static ngx_int_t
ngx_nats_test_connect(ngx_connection_t *c)
{
    int        err;
    socklen_t  len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT)  {
        if (c->write->pending_eof) {
            return NGX_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1)
        {
            err = ngx_errno;
        }

        if (err) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_nats_close_connection(ngx_nats_connection_t *nc, ngx_int_t reason,
                ngx_int_t reconnect)
{
    ngx_connection_t    *c  = nc->pc.connection;
    ngx_nats_data_t     *nd = nc->nd;
    
    if (nc->ping_timer.timer_set) {
        ngx_del_timer(&nc->ping_timer);
    }

    if (c->fd != -1) {
        ngx_close_connection(c);
    }

    if (c->read->timer_set) {
        ngx_event_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_event_del_timer(c->write);
    }

    if (!(nc->state & NGX_NATS_STATE_BYTES_EXCHANGED)) {

        ngx_log_error(NGX_LOG_DEBUG, nc->nd->log, 0,
            "cannot connect to NATS at '%s'",
            nc->server->url.data);
    }
    else {

        ngx_log_error(NGX_LOG_WARN, nc->nd->log, 0,
            "disconnected from NATS at '%s'",
            nc->server->url.data);
    }

    nd->nc = NULL;

    /* clear buffers */
    ngx_nats_buf_reset(nd->nc_read_buf);
    ngx_nats_buf_reset(nd->nc_write_buf);

    if (reconnect != 0) {
        ngx_nats_process_reconnect(nd);
    }
}


static void
ngx_nats_ping_handler(ngx_event_t *ev)
{
    ngx_nats_connection_t *nc = ev->data;

    ngx_nats_add_message(nc, "PING\r\n", 6);
    ngx_nats_flush(nc);

    ngx_add_timer(&nc->ping_timer, nc->nd->nccf->ping_interval);
}


static void
ngx_nats_check_connected(ngx_nats_connection_t *nc)
{
    if (nc->state & NGX_NATS_STATE_BYTES_EXCHANGED) {
        return;
    }

    /*
     * Notice this only means we have successfully sent to or
     * received some bytes from NATS. This does not yet mean
     * we had successfull handshake.
     */

    nc->state |= NGX_NATS_STATE_BYTES_EXCHANGED;

    if (!nc->ping_timer.timer_set) {

        nc->ping_timer.handler = ngx_nats_ping_handler;
        nc->ping_timer.log     = nc->nd->log;
        nc->ping_timer.data    = nc;

        ngx_add_timer(&nc->ping_timer, 3000);
    }

    ngx_log_error(NGX_LOG_DEBUG, nc->nd->log, 0,
        "connect() to NATS at '%s' succeeded",
        nc->server->url.data);
}


static void
ngx_nats_flush(ngx_nats_connection_t *nc)
{
    ngx_connection_t       *c;
    ngx_nats_buf_t         *buf = nc->write_buf;
    size_t                  slen;
    ssize_t                 n;

    slen = buf->end - buf->pos;
    if (slen == 0) {
        buf->pos = 0;
        buf->end = 0;
        return;
    }

    c = nc->pc.connection;

    n = c->send(c, (u_char *) (buf->buf + buf->pos), slen);

    if (n > 0) {

        ngx_nats_check_connected(nc);

        buf->pos += n;

        if (buf->pos == buf->end) {

            buf->pos = 0;
            buf->end = 0;

            if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                ngx_nats_close_connection(nc, 
                            NGX_NATS_REASON_INTERNAL_ERROR, 1);
                return;
            }
        }

        return;
    }

    if (n == NGX_ERROR) {
        ngx_nats_close_connection(nc,
                    NGX_NATS_REASON_DISCONNECTED, 1);
        return;
    }

    if (n == NGX_AGAIN) {

        /* Will need to try send later */

        ngx_add_timer(c->write, 2000);      /* TODO: configurable */

        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
            ngx_nats_close_connection(nc, NGX_NATS_REASON_INTERNAL_ERROR, 1);
            return;
        }

        return;
    }
}


static ngx_int_t
ngx_nats_add_message(ngx_nats_connection_t *nc, char *message, size_t size)
{
    ngx_int_t               rc;
    ngx_nats_buf_t         *buf = nc->write_buf;

    if (size == 0) {
        size = strlen(message);
        if (size == 0) {
            return NGX_OK;
        }
    }

    rc = ngx_nats_buf_ensure(buf, size, 1);
    if (rc != NGX_OK) {
        return rc;
    }

    ngx_memcpy(buf->buf + buf->end, message, size);
    buf->end += size;

    return NGX_OK;
}


static ngx_int_t
ngx_nats_connection_ready(ngx_nats_connection_t *nc)
{
    ngx_log_t  *log = nc->nd->log;
    ngx_int_t   n;

    ngx_nats_conn_err_reported = 0;

    /* force logging this regardless of log level */
    
    n = log->log_level;
    log->log_level = NGX_LOG_INFO;

    ngx_log_error(NGX_LOG_INFO, nc->nd->log, 0,
        "connected to NATS at '%s': version=%s id=%s",
        nc->server->url.data, 
        nc->srv_version->data,
        nc->srv_id->data);

    log->log_level = n;     /* restore log level */

    return NGX_OK;
}


static ngx_int_t
ngx_nats_parse_info(ngx_nats_connection_t *nc, char *bytes,
                ngx_nats_msg_t *msg)
{
    ngx_nats_json_value_t      *json;
    ngx_nats_json_field_t      *f;
    ngx_nats_json_object_t     *info;
    ngx_int_t                   rc, n, i;
    u_char                     *name;

    json = ngx_pcalloc(nc->pool, sizeof(ngx_nats_json_value_t));     
    if (json == NULL) {
        return NGX_ERROR;
    }
    
    rc = ngx_nats_json_parse(nc->pool, json, (char *)(bytes + msg->bstart),
                        (size_t)(msg->bend - msg->bstart));

    if (rc < 0 || json->type != NGX_NATS_JSON_OBJECT) {
        return NGX_ERROR;
    }

    info = (ngx_nats_json_object_t *) json->value.vobj;
    n = info->fields->nelts;
    f = (ngx_nats_json_field_t *) info->fields->elts;

    for (i = 0; i < n; i++, f++) {

        name = f->name.data;

        if (ngx_strcasecmp(name, (u_char *)"server_id") == 0) {
            if (f->value.type != NGX_NATS_JSON_STRING) {
                return NGX_ERROR;
            }
            nc->srv_id = f->value.value.vstr;   /* in pool */
        } 
        else if (ngx_strcasecmp(name, (u_char *)"host") == 0) {
            if (f->value.type != NGX_NATS_JSON_STRING) {
                return NGX_ERROR;
            }
            nc->srv_host = f->value.value.vstr;   /* in pool */
        }
        else if (ngx_strcasecmp(name, (u_char *)"port") == 0) {
            if (f->value.type != NGX_NATS_JSON_INTEGER) {
                return NGX_ERROR;
            }
            nc->srv_port = (ngx_int_t)f->value.value.vint;
        }
        else if (ngx_strcasecmp(name, (u_char *)"version") == 0) {
            if (f->value.type != NGX_NATS_JSON_STRING) {
                return NGX_ERROR;
            }
            nc->srv_version = f->value.value.vstr;
        }
        else if (ngx_strcasecmp(name, (u_char *)"auth_required") == 0) {
            if (f->value.type != NGX_NATS_JSON_BOOLEAN) {
                return NGX_ERROR;
            }
            nc->srv_auth_required = f->value.value.vint == 0 ? 0 : 1;
        }
        else if (ngx_strcasecmp(name, (u_char *)"ssl_required") == 0) {
            if (f->value.type != NGX_NATS_JSON_BOOLEAN) {
                return NGX_ERROR;
            }
            nc->srv_ssl_required = f->value.value.vint == 0 ? 0 : 1;
        }
        else if (ngx_strcasecmp(name, (u_char *)"max_payload") == 0) {
            if (f->value.type != NGX_NATS_JSON_INTEGER) {
                return NGX_ERROR;
            }
            nc->srv_max_payload = (ngx_int_t)f->value.value.vint;
        }
        else {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_nats_process_buffer(ngx_nats_connection_t *nc, ngx_nats_buf_t *buf)
{
    ngx_int_t       rc, skip;
    ngx_nats_msg_t  msg;
    u_char         *bytes;

    for ( ;; ) {

        if (buf->pos == buf->end) {
            ngx_nats_buf_compact(buf);
            return NGX_OK;
        }

        bytes = (u_char *)(buf->buf + buf->pos);

        rc = ngx_nats_parse(&msg, bytes, buf->end - buf->pos);

        skip = rc;      /* save it */

        if (rc <= 0) {

            if (rc == NGX_NATS_PROTO_AGAIN) {
                /* have only incomplete header */
                ngx_nats_buf_compact(buf);
                return NGX_OK;
            }

            if (rc == NGX_NATS_PROTO_AGAIN) {

                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "internal error processing NATS message");

            } else {

                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "NATS at '%s' sent invalid message, error=%d",
                    nc->server->url.data, (int)rc);

            }
                
            ngx_nats_close_connection(nc,
                        NGX_NATS_REASON_BAD_PROTOCOL, 1);

            return NGX_ERROR;
        }

        /* handle message */

        if (msg.type == NGX_NATS_MSG_OK) {

            if ((nc->state & NGX_NATS_STATE_CONNECT_OKAYED) == 0) {
                nc->state |= NGX_NATS_STATE_CONNECT_OKAYED;

                nc->state = NGX_NATS_STATE_READY;
                ngx_nats_connection_ready(nc);

            }
        }
        else if (msg.type == NGX_NATS_MSG_ERR) {

            if ((nc->state & NGX_NATS_STATE_CONNECT_SENT) &&
                !(nc->state & NGX_NATS_STATE_CONNECT_OKAYED)) {

                if (msg.bstart < msg.bend) {
                    ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                        "NATS at '%s' "
                        "returned connect error: %s",
                        nc->server->url.data, bytes+msg.bstart);
                }
                else {
                    ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                        "NATS at '%s' "
                        "returned connect error with no message",
                        nc->server->url.data);
                }

                ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_CONNECT_REFUSED, 1);

                return NGX_ERROR;
            }

            /* TODO: handle it */
        }
        else if (msg.type == NGX_NATS_MSG_PING) {

            ngx_nats_add_message(nc, "PONG\r\n", 6);

        }
        else if (msg.type == NGX_NATS_MSG_PONG) {

            /* just ignore */

        }
        else if (msg.type == NGX_NATS_MSG_INFO) {

            if (msg.bstart >= msg.bend) {
                
                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "NATS at '%s' sent INFO message with empty text",
                    nc->server->url.data);

                ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_BAD_PROTOCOL, 1);
                return NGX_ERROR;
            }

            rc = ngx_nats_parse_info(nc, (char *)bytes, &msg);

            if (rc != NGX_OK) {
                
                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "NATS at '%s' sent invalid INFO message, error=%d",
                    nc->server->url.data, (int)rc);
                
                ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_BAD_PROTOCOL, 1);
                return NGX_ERROR;
            }
        }
        else if (msg.type == NGX_NATS_MSG_MSG) {

            /* TODO: call user of this message */

        } else {

            ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                "NATS at '%s' sent unsupported message",
                nc->server->url.data);

            ngx_nats_close_connection(nc,
                        NGX_NATS_REASON_BAD_PROTOCOL, 1);

            return NGX_ERROR;
        }

        /*
        fprintf(stderr,
                "***>>> received from NATS msg '%s', skip=%d\n",
                ngx_nats_protocol_msg_name(msg.type), (int)skip);
        */

        /* skip processed message */

        buf->pos += skip;
    }
}


static void
ngx_nats_read_from_nats(ngx_connection_t *c)
{
    ngx_nats_connection_t  *nc   = c->data;
    ngx_nats_buf_t         *rbuf = nc->read_buf;
    ngx_nats_buf_t         *wbuf = nc->write_buf;
    ssize_t                 n;
    size_t                  wlen;
    ngx_int_t               rc;

    wlen = wbuf->end - wbuf->pos;

    for ( ;; ) {

        n = c->recv(c, (u_char *) (rbuf->buf + rbuf->end),
                    rbuf->cap - rbuf->end - 1);

        if (n == NGX_AGAIN) {

            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_nats_close_connection(nc,
                        NGX_NATS_REASON_INTERNAL_ERROR, 1);
                return;
            }

            break;
        }

        if (n == NGX_ERROR || n <= 0) {
            ngx_nats_close_connection(nc, NGX_NATS_REASON_DISCONNECTED, 1);
            return;
        }

        ngx_nats_check_connected(nc);

        rbuf->end += n;

        rc = ngx_nats_process_buffer(nc, rbuf);

        if (rc != NGX_OK) {
            ngx_nats_close_connection(nc, NGX_NATS_REASON_BAD_PROTOCOL, 1);
            return;
        }
    }

    /* 
     * Processing could add messages into write buffer.
     */
    if (wlen < (wbuf->end - wbuf->pos)) {
        ngx_nats_flush(nc);
    }
}


static void
ngx_nats_write_event_handler(ngx_connection_t *c)
{
    ngx_nats_connection_t  *nc = c->data;

    if (c->write->timedout) {
        ngx_nats_close_connection(nc, NGX_NATS_REASON_WRITE_TIMEOUT, 1);
        return;
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    ngx_nats_connection_init(nc);
    ngx_nats_flush(nc);
}


static void
ngx_nats_read_event_handler(ngx_connection_t *c)
{
    ngx_nats_connection_t   *nc = c->data;
    
    if (c->read->timedout) {
        ngx_nats_close_connection(nc, NGX_NATS_REASON_READ_TIMEOUT, 1);
        return;
    }

    if (ngx_nats_test_connect(c) != NGX_OK) {
        ngx_nats_close_connection(nc, NGX_NATS_REASON_CONNECT_FAILED, 1);
        return;
    }

    ngx_nats_read_from_nats(c);
}


static void
ngx_nats_connection_handler(ngx_event_t *ev)
{
    ngx_connection_t   *c = ev->data;

    if (ev->write) {
        ngx_nats_write_event_handler(c);
    }
    else {
        ngx_nats_read_event_handler(c);
    }
}


static void
ngx_nats_process_reconnect(ngx_nats_data_t *nd)
{
    if (nd->reconnect_timer.timer_set) {
        ngx_event_del_timer(&nd->reconnect_timer);
    }

    ngx_nats_connect(nd);
}


static void
ngx_nats_reconnect_handler(ngx_event_t *ev)
{
    ngx_nats_data_t *nd = ev->data;

    ngx_nats_process_reconnect(nd);
}


static void
ngx_nats_add_reconnect_timer(ngx_nats_data_t * nd)
{
    nd->reconnect_timer.handler = ngx_nats_reconnect_handler;
    nd->reconnect_timer.log     = nd->nccf->log;
    nd->reconnect_timer.data    = nd;

    if (!nd->reconnect_timer.timer_set) {

        nd->curr_index = -1;
        ngx_add_timer(&nd->reconnect_timer, nd->nccf->reconnect_interval);
    }
}


static ngx_int_t
ngx_nats_get_peer(ngx_peer_connection_t *pc, void *data)
{
    /* Must exist but I don't use it. */
    return NGX_OK;
}


static void
ngx_nats_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    /* Must exist but I don't use it. */
}

static void
ngx_nats_connection_init(ngx_nats_connection_t *nc)
{
    u_char  connstr[1024];
    u_char *p;

    p = ngx_sprintf(connstr,
            "CONNECT {\"verbose\":false,\"pedantic\":false,"
            "\"user\":\"%V\",\"pass\":\"%V\"}\r\n",
            &nc->nd->nccf->user, &nc->nd->nccf->password);

    if ((nc->state & NGX_NATS_STATE_CONNECT_SENT) == 0) {

        nc->state |= NGX_NATS_STATE_CONNECT_SENT;

        ngx_nats_add_message(nc, (char*)connstr, (p - connstr));

        return;
    }

}


static ngx_int_t
ngx_nats_connect_loop(ngx_nats_data_t *nd)
{
    ngx_nats_core_conf_t   *nccf;
    ngx_nats_connection_t  *nc;
    ngx_nats_server_t      *ns;
    ngx_addr_t             *a;
    ngx_int_t               rc, n;
    ngx_connection_t       *c = NULL;

    nccf = nd->nccf;

    n = nccf->servers->nelts;

    if (nd->curr_index < 0) {

        /* new reconnect loop */
        nd->curr_index = nd->last_index;
        nd->nconnects++;

    } else {

        if (++nd->curr_index >= n) {
            nd->curr_index = 0;
        }

        if (nd->curr_index == nd->last_index) {

            /*
             * means we tried each server and could not connect
             * to any of them, now sleep and then repeat.
             */

            if (!ngx_nats_conn_err_reported) {

                ngx_nats_conn_err_reported = 1;

                ngx_log_error(NGX_LOG_ERR, nd->log, 0,
                    "cannot connect to NATS server%s, "
                    "will try every %d milliseconds",
                    (u_char*)(n > 1 ? "s" : ""),
                    (int)nccf->reconnect_interval);
            }

            ngx_nats_add_reconnect_timer(nd);

            return NGX_DECLINED;
        }
    }

    ngx_reset_pool(nd->nc_pool);
    ngx_nats_buf_reset(nd->nc_read_buf);
    ngx_nats_buf_reset(nd->nc_write_buf);

    nc = ngx_pcalloc(nd->nc_pool, sizeof(ngx_nats_connection_t));
    if (nc == NULL) {
        return NGX_ERROR;
    }

    nd->nc = nc;
    nc->nd = nd;

    ns = (ngx_nats_server_t *) nccf->servers->elts;
    ns = ns + nd->curr_index;

    a = ns->addrs;  /* TODO: handle multiple addrs? */

    nc->pc.data         = nd;
    nc->pool            = nd->nc_pool;
    nc->read_buf        = nd->nc_read_buf;
    nc->write_buf       = nd->nc_write_buf;
    nc->pc.log          = nccf->log;
    nc->pc.sockaddr     = a->sockaddr;
    nc->pc.socklen      = a->socklen;
    nc->pc.name         = &ns->url;
    nc->pc.tries        = 1;
    nc->pc.get          = ngx_nats_get_peer;
    nc->pc.free         = ngx_nats_free_peer;

    nc->server = ns;

    rc = ngx_event_connect_peer(&nc->pc);

    if (rc == NGX_BUSY || rc == NGX_ERROR || rc == NGX_DECLINED) {
        return NGX_AGAIN;
    }

    if (rc == NGX_OK || rc == NGX_AGAIN) {

        c = nc->pc.connection;

        c->data = nc;

        c->write->handler   = ngx_nats_connection_handler;
        c->read->handler    = ngx_nats_connection_handler;

        c->log              = nd->log;
        
        c->read->log        = c->log;
        c->write->log       = c->log;
        /* TODO: do I need SSL? c->pool is for SSL only. */
        if (c->pool != NULL)
            c->pool->log = c->log;
    }

    if (rc == NGX_AGAIN) {
        ngx_add_timer(c->write, 2000);  /* TODO: configurable? */
        return NGX_OK;
    }

    if (rc == NGX_OK) {
        /* Connected right here. */
        ngx_nats_connection_init(nc);
        ngx_nats_flush(nc);
        return NGX_OK;
    }

    return NGX_OK;
}

static void
ngx_nats_connect(ngx_nats_data_t *nd)
{
    ngx_int_t   rc;

    for ( ;; ) {

        /*
         * If this returns NGX_AGAIN then must call again to try
         * another server. Otherwise break because we either connected
         * or all servers failed and the retry timer was set.
         */
        rc = ngx_nats_connect_loop(nd);

        if (rc != NGX_AGAIN) {
            break;
        }
    }
}

/*
 * Called on worker process init. Initiates connecting to NATS.
 */
ngx_int_t
ngx_nats_init(ngx_nats_core_conf_t *nccf)
{
    ngx_nats_data_t  *nd;

    nd = (ngx_nats_data_t *) nccf->data;
    
    nd->log = nccf->log;

    /* 
     * Try to connect to any NATS in the list.
     * If fails it'll setup the retry timer, etc.
     */
    ngx_nats_connect(nd);

    return NGX_OK;
}



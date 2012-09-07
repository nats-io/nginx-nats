/*
 * Copyright 2012 Apcera Inc. All rights reserved.
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
 * Forward declarations.
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

ngx_nats_data_t *ngx_nats_data = NULL;

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

    if (buf->cap >= NGX_NATS_MAX_MESSAGE_SIZE) {
        ngx_log_error(NGX_LOG_CRIT, buf->log, 0,
        "attempt to increase NATS buffer size to more than maximum of %i",
        (ngx_int_t)NGX_NATS_MAX_MESSAGE_SIZE);
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
    ngx_connection_t   *c  = nc->pc.connection;
    ngx_nats_data_t    *nd = nc->nd;
    ngx_nats_client_t **pclient;
    ngx_int_t           i, n, immediate;

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

    immediate = 0;

    if (!(nc->state & NGX_NATS_STATE_BYTES_EXCHANGED)) {

        /* reconnect immediately because we simply could not connect */
        immediate = 1;

        ngx_log_error(NGX_LOG_DEBUG, nc->nd->log, 0,
            "cannot connect to NATS at '%s'",
            nc->server->url.data);

    } else if (nc->state == NGX_NATS_STATE_READY) {

        ngx_log_error(NGX_LOG_WARN, nc->nd->log, 0,
            "disconnected from NATS at '%s'",
            nc->server->url.data);

        /* Call disconnected in clients */

        n       = nd->cd.clients.nelts;
        pclient = nd->cd.clients.elts;

        for (i = 0; i < n; i++, pclient++) {
            if ((*pclient)->disconnected) {
                (*pclient)->disconnected(*pclient);
            }
        }

    } else {

        /* TODO: handle partial connect */

    }

    nd->cd.subs.nelts = 0;      /* remove all subscriptions */

    nd->nc = NULL;

    /* clear buffers */
    ngx_nats_buf_reset(nd->nc_read_buf);
    ngx_nats_buf_reset(nd->nc_write_buf);

    if (reconnect != 0) {

        /*
         * if we could not connect at all or simply disconnected
         * then try to reconnect immediately.
         * If we did connect and connection broke because of the internal
         * error or bad message from NATS then wait before reconnecting
         * so a poison pill message, or we're out of memory, do not put
         * us into a tight connection loop.
         */
        
        if (reason == NGX_NATS_REASON_DISCONNECTED || immediate) {

            /* this reconnects immediately */
            ngx_nats_process_reconnect(nd);

        } else {

            /* this runs timer, then reconnects */
            ngx_nats_add_reconnect_timer(nd);

        }
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
     * we had successful handshake.
     */

    nc->state |= NGX_NATS_STATE_BYTES_EXCHANGED;

    if (!nc->ping_timer.timer_set) {

        nc->ping_timer.handler = ngx_nats_ping_handler;
        nc->ping_timer.log     = nc->nd->log;
        nc->ping_timer.data    = nc;

        ngx_add_timer(&nc->ping_timer, nc->nd->nccf->ping_interval);
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

        ngx_add_timer(c->write, 5000);      /* TODO: configurable */

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
    ngx_nats_data_t    *nd = nc->nd;
    ngx_log_t          *log = nd->log;
    ngx_nats_client_t **pclient;
    ngx_int_t           i, n;

    ngx_nats_conn_err_reported = 0;

    /* force logging this regardless of log level */
    
    n = log->log_level;
    log->log_level = NGX_LOG_INFO;

    ngx_log_error(NGX_LOG_INFO, nd->log, 0,
        "nats: connected to NATS at '%s': version='%s'",
        nc->server->url.data, 
        nc->srv_version->data,
        nc->srv_id->data);

    log->log_level = n;     /* restore log level */

    /* Call connected in clients */

    n       = nd->cd.clients.nelts;
    pclient = nd->cd.clients.elts;

    for (i = 0; i < n; i++, pclient++) {
        (*pclient)->connected(*pclient);
    }

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

static void
ngx_nats_process_msg(ngx_nats_connection_t *nc, ngx_nats_buf_t *buf,
                ngx_nats_msg_t *msg)
{
    ngx_int_t                   n, sid;
    ngx_nats_subscription_t   **psub;
    ngx_nats_subscription_t    *sub;
    ngx_str_t                  *r = NULL;

    sid  = msg->sid;
    n    = nc->nd->cd.subs.nelts;
    psub = nc->nd->cd.subs.elts;

    /* could receive rogue message? */
    if (sid < 0 || sid >= n) {
        return;
    }

    sub = psub[sid];
    if (sub == NULL) {
        return;
    }

    if (msg->replyto.len > 0) {
        r = &msg->replyto;
    }

    sub->handle_msg(sub->client, &msg->subject, sid, r, 
                (u_char *) (buf->buf + buf->pos + msg->bstart),
                (msg->bend - msg->bstart) );
}


static ngx_int_t
ngx_nats_process_buffer(ngx_nats_connection_t *nc, ngx_nats_buf_t *buf)
{
    ngx_int_t       rc, skip;
    ngx_nats_msg_t  msg;
    u_char         *bytes;
    char           *ce;

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

                /* have incomplete message */

                ngx_nats_buf_compact(buf);

                if (buf->end == buf->cap) {
                    
                    /*
                     * this means we have full buffer but it doesn't fit
                     * one message, so need to grow the buffer.
                     * TODO: not crucial but if it is MSG message then
                     * we may know by how much to grow the buffer.
                     * I don't have it now so will double the buffer,
                     * possibly several times, but it'll happen only until
                     * the buffer gorws enough, so is OK.
                     */
                    
                    if (nc->srv_max_payload > 0 && 
                                buf->cap >= (size_t)nc->srv_max_payload) {

                        /* NATS sent message larger than promised */
                        ngx_log_error(NGX_LOG_CRIT, nc->nd->log, 0,
                            "NATS sent message larger than may payload of %i",
                            nc->srv_max_payload);
            
                        ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_BAD_PROTOCOL, 1);

                        return NGX_ERROR;
                    }
                    
                    if (buf->cap >= NGX_NATS_MAX_MESSAGE_SIZE) {

                        /* TODO: check max_payload mot more than 256MB? */

                        ngx_log_error(NGX_LOG_CRIT, nc->nd->log, 0,
                            "NATS sent message larger than 256MB");
            
                        ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_BAD_PROTOCOL, 1);

                        return NGX_ERROR;
                    }
                    
                    /* this will double the buf */
                    rc = ngx_nats_buf_ensure(buf, buf->cap - 1, 0);
                    if (rc != NGX_OK) {
                        /* out of memory */
                        ngx_log_error(NGX_LOG_CRIT, nc->nd->log, 0,
                            "out of memory receiving NATS message");
            
                        ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_NO_MEMORY, 1);

                        return NGX_ERROR;
                    }
                }

                return NGX_OK;
            }

            if (rc == NGX_NATS_PROTO_ERR_ERROR) {

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

            /* ignore all OKs */

        }
        else if (msg.type == NGX_NATS_MSG_ERR) {

            ce = "";
            
            if ((nc->state & NGX_NATS_STATE_CONNECT_SENT) &&
                !(nc->state & NGX_NATS_STATE_CONNECT_OKAYED)) {
                ce = " connect";
            }

            if (msg.bstart < msg.bend) {
                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "NATS at '%s' "
                    "returned&s error: %s",
                    ce, nc->server->url.data, bytes+msg.bstart);
            }
            else {
                ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                    "NATS at '%s' "
                    "returned&s error with no message",
                    ce, nc->server->url.data);
            }

            if ((nc->state & NGX_NATS_STATE_CONNECT_SENT) &&
                !(nc->state & NGX_NATS_STATE_CONNECT_OKAYED)) {

                ngx_nats_close_connection(nc,
                            NGX_NATS_REASON_CONNECT_REFUSED, 1);

                return NGX_ERROR;
            }

            /* TODO: what am I supposed to do about it? */

        }
        else if (msg.type == NGX_NATS_MSG_PING) {

            ngx_nats_add_message(nc, "PONG\r\n", 6);

        }
        else if (msg.type == NGX_NATS_MSG_PONG) {

            if ((nc->state & NGX_NATS_STATE_CONNECT_OKAYED) == 0) {
                nc->state |= NGX_NATS_STATE_CONNECT_OKAYED;

                nc->state = NGX_NATS_STATE_READY;

                ngx_nats_connection_ready(nc);
            }
            
            /* otherwise just ignore */
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

            ngx_nats_process_msg(nc, buf, &msg);

        } else {

            ngx_log_error(NGX_LOG_ERR, nc->nd->log, 0,
                "NATS at '%s' sent unsupported message",
                nc->server->url.data);

            ngx_nats_close_connection(nc,
                        NGX_NATS_REASON_BAD_PROTOCOL, 1);

            return NGX_ERROR;
        }

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
    /* 128 is more than hardcoded string below. Increase when need. */
    u_char  connstr[NGX_NATS_MAS_USER_PASS_LEN + 128];
    u_char *p;

    p = ngx_sprintf(connstr,
            "CONNECT {\"verbose\":false,\"pedantic\":false,"
            "\"user\":\"%V\",\"pass\":\"%V\"}\r\nPING\r\n",
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
        ngx_add_timer(c->write, 5000);  /* TODO: configurable? */
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

    ngx_nats_data = nd;
    
    nd->log = nccf->log;

    /* 
     * Try to connect to any NATS in the list.
     * If fails it'll setup the retry timer, etc.
     */
    ngx_nats_connect(nd);

    return NGX_OK;
}

void
ngx_nats_exit(ngx_nats_core_conf_t *nccf)
{
    ngx_nats_data = NULL;
}

/*---------------------------------------------------------------------------
 * Client functions.
 *--------------------------------------------------------------------------*/

ngx_int_t
ngx_nats_add_client(ngx_nats_client_t *client)
{
    ngx_nats_data_t            *nd = ngx_nats_data;
    ngx_nats_client_data_t     *cd;
    ngx_nats_client_t         **c;

    if (nd == NULL) {
        return NGX_ABORT;   /* nats not defined in the config */
    }

    cd = &nd->cd;

    c = ngx_array_push(&cd->clients);
    if (c == NULL) {
        return NGX_ERROR;
    }

    *c = client;

    if (nd->nc == NULL) {
        return NGX_OK;
    }

    if (nd->nc->state != NGX_NATS_STATE_READY) {
        return NGX_OK;
    }

    client->connected(client);

    return NGX_OK;
}


ngx_int_t
ngx_nats_publish(ngx_nats_client_t *client, ngx_str_t *subject,
            ngx_str_t *replyto, u_char *data, ngx_uint_t len)
{
    ngx_nats_data_t        *nd = ngx_nats_data;
    ngx_nats_connection_t  *nc;
    u_char                  header[512+64];   /* TODO: !! */
    u_char                 *p;
    ngx_int_t               rc;

    if (nd == NULL) {
        return NGX_ABORT;   /* nats not defined in the config */
    }

    if (nd->nc == NULL) {
        return NGX_ERROR;       /* not connected    */
    }

    nc = nd->nc;

    if (nc->state != NGX_NATS_STATE_READY) {
        return NGX_ERROR;       /* not connected    */
    }

    if (replyto != NULL) {
        if (subject->len + replyto->len > 512) {
            return NGX_DECLINED;
        }
        p = ngx_sprintf(header,
                "PUB %s %s %ui\r\n",
                subject->data, replyto->data, len);
    } else {
        if (subject->len > 512) {
            return NGX_DECLINED;
        }
        p = ngx_sprintf(header,
                "PUB %s %ui\r\n",
                subject->data, len);
    }

    rc = ngx_nats_add_message(nc, (char*)header, (p - header));
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_nats_add_message(nc, (char *)data, (size_t)len);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_nats_add_message(nc, "\r\n", 2);
    if (rc != NGX_OK) {
        return rc;
    }

    ngx_nats_flush(nd->nc);

    return NGX_OK;
}


ngx_int_t
ngx_nats_subscribe(ngx_nats_client_t *client, ngx_str_t *subject,
                ngx_nats_handle_msg_pt handle_msg)
{
    ngx_nats_data_t            *nd = ngx_nats_data;
    ngx_nats_connection_t      *nc;
    ngx_nats_client_data_t     *cd;
    ngx_nats_subscription_t    *sub;
    ngx_nats_subscription_t   **psub;
    u_char                      header[512+64];   /* TODO: !! */
    u_char                     *p;
    ngx_int_t                   sid, rc;

    if (nd == NULL) {
        return NGX_ABORT;   /* nats not defined in the config */
    }

    if (nd->nc == NULL) {
        return NGX_ERROR;       /* not connected    */
    }

    nc = nd->nc;

    if (nc->state != NGX_NATS_STATE_READY) {
        return NGX_ERROR;       /* not connected    */
    }

    cd = &nd->cd;

    sid = cd->subs.nelts;

    sub = ngx_pcalloc(nd->nc_pool, sizeof(ngx_nats_subscription_t));
    if (sub == NULL) {
        return NGX_ERROR;
    }

    sub->client     = client;
    sub->handle_msg = handle_msg;
    sub->sid        = sid;

    psub = ngx_array_push(&cd->subs);
    if (psub == NULL) {
        return NGX_ERROR;
    }

    *psub = sub;

    /* no queue support for now... */
    p = ngx_sprintf(header,
            "SUB %s %ui\r\n",
            subject->data, sid);
	
    rc = ngx_nats_add_message(nc, (char*)header, (p - header));
    if (rc != NGX_OK) {
        return rc;
    }

    ngx_nats_flush(nd->nc);

    return sid;
}

static uint32_t
_nats_rand4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) 
{
    return ((((a * 31) + b) * 31) + c) * 31 + d;
}

/* TODO: change impl when we'll get SFMT? */
ngx_int_t
ngx_nats_create_inbox(u_char *buf, size_t bufsize)
{
    ngx_time_t     *tp;
    ngx_addr_t     *local_ip;
    u_char         *pend;
    size_t          i;
    uint32_t        partA, partB, partC, partD, ipvar;
    uint32_t        r1, r2, r3, r4;

    if (bufsize < 34) {
        return NGX_ERROR;
    }

    ngx_time_update();
    tp = ngx_timeofday();

    local_ip = ngx_nats_get_local_ip();

    ipvar = 0;
    
    if (local_ip != NULL) {
        for (i = 0; i < local_ip->name.len; i++) {
            ipvar = (ipvar * 31) + (uint32_t)local_ip->name.data[i];
        }
    }

    /* Nginx seeds random to same value in all workers *after*
     * it calls process_init for user modules. Then all workers
     * get same random sequence. I need to look more into it,
     * but generally I need my own random not depending on Nginx.
     * Then I won't have to init it every time here. Performance is not
     * an issue but I still shouldn't do it.
     */
    ngx_nats_seed_random();

    r1 = (uint32_t) ngx_random();
    r2 = (uint32_t) ngx_random();
    r3 = (uint32_t) ngx_random();
    r4 = (uint32_t) ngx_random();

    partA = _nats_rand4(ipvar, r1, (uint32_t)ngx_pid, (uint32_t)tp->msec);
    partB = _nats_rand4(ipvar, r2, (uint32_t)ngx_pid, (uint32_t)tp->sec);
    partC = _nats_rand4(ipvar, r3, (uint32_t)tp->sec, (uint32_t)tp->msec);
    partD = (uint32_t) (r4 & 0x00ff);   /* 1 byte only */
    
    pend = ngx_sprintf(buf, "_INBOX.%08xD%08xD%08xD%02xD", partA, partB, partC, partD);

    *pend = 0;

    return (ngx_int_t)(pend - buf);
}


/*
 * Copyright (C) Apcera Inc.
 *
 */

#ifndef _NGX_NATS_PROTOCOL_H_INCLUDED_
#define _NGX_NATS_PROTOCOL_H_INCLUDED_

#include <ngx_core.h>


/* 
 * Must be negative.
 */
#define NGX_NATS_PROTO_AGAIN                (-1)    /* incomplete message   */
#define NGX_NATS_PROTO_ERR_ERROR            (-2)    /* no memory etc.       */
#define NGX_NATS_PROTO_ERR_INVALID_HEADER   (-3)
#define NGX_NATS_PROTO_ERR_INVALID_MSG      (-4)
#define NGX_NATS_PROTO_ERR_INVALID_JSON     (-5)


/*
 * Types of protocol messages sent by NATS server to the client.
 */
#define NGX_NATS_MSG_OK         (1)     /* "+OK" messge, not OK return code */
#define NGX_NATS_MSG_ERR        (2)
#define NGX_NATS_MSG_PING       (3)
#define NGX_NATS_MSG_PONG       (4)
#define NGX_NATS_MSG_INFO       (5)
#define NGX_NATS_MSG_MSG        (6)


typedef struct {

    ngx_int_t       type;

    /* For "MSG" */
    ngx_int_t       sid;        /* sids are integers only           */
    ngx_int_t       len;        /* payload length                   */
    ngx_str_t       replyto;    /* replyTo.len==0 means no replyTo  */

    /*
     * Used for "INFO", "-ERR" and "MSG". "+OK", "PING" and "PONG"
     * have no body. "-ERR" body is a string in quotes.
     * "INFO" body is a JSON. "MSG" body is anything.
     * There is a 0 after bend if the caller needs it.
     */
    size_t          bstart;     /* start of message body            */
    size_t          bend;       /* end of message body              */

} ngx_nats_msg_t;


char* ngx_nats_protocol_msg_name(ngx_int_t type);

/* See comments in ngx_nats_protocol.c for meaning of return value. */
ngx_int_t ngx_nats_parse(ngx_nats_msg_t *msg, u_char* s, size_t len);


#endif /* _NGX_NATS_PROTOCOL_H_INCLUDED_ */



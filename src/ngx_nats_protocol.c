/*
 * Copyright (C) Apcera Inc.
 *
 */

#include "ngx_nats_protocol.h"


/*
 * This parses incoming NATS messages.
 */

/*---------------------------------------------------------------------------
 * Forward declarations of functions.
 *--------------------------------------------------------------------------*/

/*
 * Top-level function "ngx_nats_parse(...)" and all below return:
 *     - If return value is positive then it's the total number of bytes
 *           in a message and "msg" is formed.
 *     - If returns NGX_NATS_PROTO_AGAIN then message is incomplete,
 *           caller must read more bytes from NATS and call parse again.
 *     - Otherwise an error NGX_NATS_PROTO_ERR_...., connection to NATS
 *           was closed because of protocol violation.
 */

static ngx_int_t ngx_nats_parse_ok(ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);
static ngx_int_t ngx_nats_parse_err(ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);
static ngx_int_t ngx_nats_parse_ping(ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);
static ngx_int_t ngx_nats_parse_pong(ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);
static ngx_int_t ngx_nats_parse_info(ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);
static ngx_int_t ngx_nats_parse_msg (ngx_nats_msg_t *m, size_t hlen,
                    u_char* s, size_t len);


#define _NATS_PARSE_ERR ((size_t)-1)

/*---------------------------------------------------------------------------
 * Implementations.
 *--------------------------------------------------------------------------*/

char*
ngx_nats_protocol_msg_name(ngx_int_t type)
{
    switch(type) {
    case NGX_NATS_MSG_OK:       return "+OK";
    case NGX_NATS_MSG_ERR:      return "-ERR";
    case NGX_NATS_MSG_PING:     return "PING";
    case NGX_NATS_MSG_PONG:     return "PONG";
    case NGX_NATS_MSG_INFO:     return "INFO";
    case NGX_NATS_MSG_MSG:      return "MSG";
    }
    return "<invalid message type>";
}


static ngx_int_t
_nats_atoi(u_char *s, size_t n, ngx_int_t *result)
{
    ngx_int_t  value;

    if (n == 0) {
        return NGX_ERROR;
    }

    for (value = 0; n--; s++) {
        if (*s < '0' || *s > '9') {
            return NGX_ERROR;
        }

        value = value * 10 + (*s - '0');
    }

    *result = value;
    return NGX_OK;
}

static size_t
_nats_token(u_char *s, size_t pos, size_t max, size_t *ns, size_t *ne)
{
    char q = 0;
    
    *ns = max;
    *ne = max;
    
    /* skip spaces */
    for ( ; pos < max && s[pos] == ' '; pos++);
    if (pos == max) {
        return max;
    }

    *ns = pos;
    if (s[pos] == '\'' || s[pos] == '\"') {
        if (pos >= max-1) {
            return _NATS_PARSE_ERR;
        }
        q = s[pos++];
        for ( ;; ) {
            if (s[pos] == '\\') {
                if (pos >= max-1) {
                    return _NATS_PARSE_ERR;
                }
                pos += 2;
            } else if (s[pos] == q) {
                *ne = pos+1;
                return pos + 1;
            } else if (pos >= max-1) {
                return _NATS_PARSE_ERR;
            } else {
                pos++;
            }
        }
    }

    for ( ; pos < max && s[pos] != ' ' && s[pos] != '\r'; pos++);
    
    *ne = pos;
    return pos;
}


ngx_int_t
ngx_nats_parse(ngx_nats_msg_t *m, u_char* s, size_t len)
{
    size_t      p, hlen;

    ngx_memzero(m, sizeof(ngx_nats_msg_t));

    /*
     * Parse header if possible and then call appropriate parser.
     * May not have enough bytes to even parse the header.
     */

    for (p = 0; p < len; p++) {

        if (s[p] == '\n') {
            return NGX_NATS_PROTO_ERR_INVALID_HEADER;
        }

        if (s[p] == ' ' || s[p] == '\r') {
            break;
        }
    }

    if (p >= len) {

        /*
         * TODO: return error if no complete header in first 4096 bytes.
         * Only "-ERR" may be quite long due to error message, others can't.
         */

        return NGX_NATS_PROTO_AGAIN;
    }

    hlen = p;        /* header length */

    if (p == 3) {

        if (ngx_strncasecmp(s, (u_char *) "+OK", 3) == 0) {
            return ngx_nats_parse_ok(m, hlen, s, len);
        }

        if (ngx_strncasecmp(s, (u_char *) "MSG", 3) == 0) {
            return ngx_nats_parse_msg(m, hlen, s, len);
        }

        return NGX_NATS_PROTO_ERR_INVALID_HEADER;
    }

    if (p == 4) {

        if (ngx_strncasecmp(s, (u_char *) "-ERR", 4) == 0) {
            return ngx_nats_parse_err(m, hlen, s, len);
        }

        if (ngx_strncasecmp(s, (u_char *) "PING", 4) == 0) {
            return ngx_nats_parse_ping(m, hlen, s, len);
        }

        if (ngx_strncasecmp(s, (u_char *) "PONG", 4) == 0) {
            return ngx_nats_parse_pong(m, hlen, s, len);
        }

        if (ngx_strncasecmp(s, (u_char *) "INFO", 4) == 0) {
            return ngx_nats_parse_info(m, hlen, s, len);
        }

        return NGX_NATS_PROTO_ERR_INVALID_HEADER;
    }

    /* Modify above and here if more NATS message types. */

    return NGX_NATS_PROTO_ERR_INVALID_HEADER;
}


ngx_int_t
ngx_nats_parse_ok(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "+OK\r\n"
     */

    m->type = NGX_NATS_MSG_OK;

    if (len < 4) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[3] != '\r') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    if (len < 5) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[4] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    return 5;
}


ngx_int_t
ngx_nats_parse_ping(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "PING\r\n"
     */

    m->type = NGX_NATS_MSG_PING;

    if (len < 5) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[4] != '\r') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    if (len < 6) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[5] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    return 6;
}


ngx_int_t
ngx_nats_parse_pong(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "PONG\r\n"
     */

    m->type = NGX_NATS_MSG_PONG;

    if (len < 5) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[4] != '\r') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    if (len < 6) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[5] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    return 6;
}


ngx_int_t
ngx_nats_parse_err(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "-ERR error-text\r\n".
     *
     * "error-text" is optional (???)
     */

    size_t  n;

    m->type = NGX_NATS_MSG_ERR;

    for (n = hlen; n < len && s[n] == ' '; n++);

    if (n >= len) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (n == hlen) {

        /* Must be \r\n, if possible that NATS sends -ERR with no message */
        if (s[n] != '\r') {
            return NGX_NATS_PROTO_ERR_INVALID_MSG;
        }

        if (n == len - 1) {
            return NGX_NATS_PROTO_AGAIN;
        }

        if (s[n+1] != '\n') {
            return NGX_NATS_PROTO_ERR_INVALID_MSG;
        }

        m->bstart = hlen;
        m->bend   = hlen;
        s[m->bend] = 0;

        return (ngx_int_t)(n + 2);
    }

    m->bstart = n;      /* first non-space char after -ERR */

    for (n++; n < len && s[n] != '\r'; n++);

    /* Means we didn't find or found as very last available char */
    if (n >= len-1) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[n+1] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    m->bend = n;
    s[m->bend] = 0;

    return (ngx_int_t)(n + 2);
}


ngx_int_t
ngx_nats_parse_info(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "INFO {...fields...}\r\n"
     */

    /* TODO: return error if no complete header in first 1024 bytes */

    size_t  n;

    m->type = NGX_NATS_MSG_INFO;

    for (n = hlen; n < len && s[n] == ' '; n++);

    if (n >= len) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (n == hlen) {     /* no space(s) after INFO */
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    m->bstart = n;      /* first non-space char after INFO */

    for (n++; n < len && s[n] != '\r'; n++);

    /* Means we didn't find or found as very last available char */
    if (n >= len-1) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[n+1] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    m->bend = n;
    s[m->bend] = 0;

    return (ngx_int_t)(n + 2);
}



ngx_int_t
ngx_nats_parse_msg(ngx_nats_msg_t *m, size_t hlen, u_char* s, size_t len)
{
    /*
     * "MSG Subject Sid [reply-to] Len\r\n...payload...\r\n".
     * "reply-to" is optional, may not have spaces.
     * We do have at least one char after "MSG".
     */

    /* TODO: return error if no complete header in first 1024 bytes */

    size_t      n, max, ns, ne, ns2, ne2;
    ngx_int_t   rc;

    m->type = NGX_NATS_MSG_MSG;

    if (s[hlen] != ' ') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    for (n = hlen; n < len-1 && s[n] != '\r'; n++);
    if (n >= len-1) {
        return NGX_NATS_PROTO_AGAIN;
    }
    if (s[n+1] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    /* have full header */
    max = n;

    n = hlen;

    /* get subject */
    n = _nats_token(s, n, max, &ns, &ne);
    if (n == _NATS_PARSE_ERR || n == max || ns >= ne) {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }
    m->subject.data = (u_char *)(s + ns);
    m->subject.len  = ne - ns;

    /* get Sid */
    n = _nats_token(s, n, max, &ns, &ne);
    if (n == _NATS_PARSE_ERR || n == max || ns >= ne) {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }
    rc = _nats_atoi((u_char *)(s + ns), ne - ns, &m->sid);
    if (rc != NGX_OK) {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    /* get next one or two tokens */
    n = _nats_token(s, n, max, &ns, &ne);
   if (n == _NATS_PARSE_ERR || ns >= ne) {     /* may be last token */
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    if (n < max) { /* may have one more token */
        n = _nats_token(s, n, max, &ns2, &ne2);
        if (n == _NATS_PARSE_ERR) {
            return NGX_NATS_PROTO_ERR_INVALID_MSG;
        }
        if (ns2 < ne2) {
            /* have token */
            m->replyto.data = (u_char *)(s + ns);
            m->replyto.len  = ne - ns;
            ns = ns2;
            ne = ne2;
        }
        /* check no more tokens */
        n = _nats_token(s, n, max, &ns2, &ne2);
        if (n == _NATS_PARSE_ERR || n < max || ns2 < ne2) {
            return NGX_NATS_PROTO_ERR_INVALID_MSG;
        }
    }

    rc = _nats_atoi((u_char *)(s + ns), ne - ns, &m->len);
    if (rc != NGX_OK) {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    ns = max + 2;   /* start of payload */

    if ((ns + m->len + 2) > len) {
        return NGX_NATS_PROTO_AGAIN;
    }

    if (s[ns + m->len] != '\r' || s[ns + m->len + 1] != '\n') {
        return NGX_NATS_PROTO_ERR_INVALID_MSG;
    }

    m->bstart = ns;
    m->bend   = ns + m->len;

    m->subject.data[m->subject.len] = 0;

    if (m->replyto.len > 0) {
        m->replyto.data[m->replyto.len] = 0;
    }

    s[m->bend] = 0;

    return (ngx_int_t) (ns + m->len + 2);
}



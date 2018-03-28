/*
 * Copyright 2012-2018 (C) The NATS Authors.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "ngx_nats_json.h"


/*
 * JSON parsing. Doesn't support \u in strings otherwise should be complete.
 * Fields in object are kept in an array as opposed to normaly as a hashtable.
 * This is for first version to bootstrap it, using ngx_hash is a little
 * complicated. Works OK for limited No of fields in objects.
 * Need to revisit this.
 */


typedef struct
{
    ngx_pool_t     *pool;

    u_char         *text;           /* JSON string                          */
    size_t          len;            /* length of string in "s"              */
    size_t          pos;            /* current parse position               */
    size_t          istart;         /* start position of the item we parse  */

    u_char         *str;            /* for parsing string values            */
    size_t          str_cap;        /* allocated size (capacity)            */
    size_t          str_pos;

    size_t          err_pos;

} ngx_nats_json_parse_ctx_t;


/*---------------------------------------------------------------------------
 * Forward declarations.
 *--------------------------------------------------------------------------*/

static ngx_int_t ngx_nats_json_parse_object(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_object_t *o);
static ngx_int_t ngx_nats_json_parse_array(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_array_t *a);
static ngx_int_t ngx_nats_json_parse_string(ngx_nats_json_parse_ctx_t *pc,
                    ngx_str_t *s, u_char q);
static ngx_int_t ngx_nats_json_parse_hex(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_value_t *v);
static ngx_int_t ngx_nats_json_parse_double(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_value_t *v);
static ngx_int_t ngx_nats_json_parse_int(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_value_t *v);
static ngx_int_t ngx_nats_json_parse_value(ngx_nats_json_parse_ctx_t *pc,
                    ngx_nats_json_value_t *v);


/*---------------------------------------------------------------------------
 * Implementations.
 *--------------------------------------------------------------------------*/

const u_char *
ngx_nats_json_type_name(ngx_int_t type)
{
    switch(type)
    {
    case NGX_NATS_JSON_NULL:        return (u_char *)"Null";
    case NGX_NATS_JSON_OBJECT:      return (u_char *)"Object";
    case NGX_NATS_JSON_ARRAY:       return (u_char *)"Array";
    case NGX_NATS_JSON_BOOLEAN:     return (u_char *)"Boolean";
    case NGX_NATS_JSON_INTEGER:     return (u_char *)"Integer";
    case NGX_NATS_JSON_DOUBLE:      return (u_char *)"Double";
    case NGX_NATS_JSON_STRING:      return (u_char *)"String";
    default:                        return (u_char *)"<invalid JSON type>";
    }
}


static ngx_int_t
_skipSpaces(ngx_nats_json_parse_ctx_t *pc)
{
    u_char     *t;
    size_t      p;
    size_t      m = pc->len;

    t = pc->text;
    pc->istart = -1;

    for (p = pc->pos; p < m; p++) {
        u_char c = t[p];
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
            pc->pos = p;
            return 0;
        }
    }

    pc->err_pos = p;
    return NGX_NATS_JSON_ERR_EOF;
}

static ngx_int_t
ngx_nats_json_parse_object(ngx_nats_json_parse_ctx_t *pc,
        ngx_nats_json_object_t *o)
{
    /* pc->pos is right after '{' */

    ngx_nats_json_field_t  *f;
    u_char                 *t;
    ngx_int_t               rc;
    size_t                  psave = 0;
    u_char                  c, need_field=0;

    t = pc->text;
    pc->istart = pc->pos - 1;

    while(pc->pos < pc->len) {

        psave = pc->pos;

        rc = _skipSpaces(pc);
        if (rc != 0) {
            pc->err_pos = psave;
            return NGX_NATS_JSON_ERR_EOF;
        }

        pc->istart = pc->pos;
        c = t[pc->pos++];

        if (c == '}') {
            if (need_field) {
                pc->err_pos = pc->pos - 1;
                return NGX_NATS_JSON_ERR_SYNTAX;
            }
            return 0;
        }


        /* must have field name in quotes */
        if (c != '\'' && c != '\"') {
            pc->err_pos = pc->pos - 1;
            return NGX_NATS_JSON_ERR_SYNTAX;
        }

        f = (ngx_nats_json_field_t *) ngx_array_push(o->fields);
        if (f == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }
        ngx_memzero(f, sizeof(ngx_nats_json_field_t));

        rc = ngx_nats_json_parse_string(pc, &f->name, c);
        if (rc != 0) {
            return rc;
        }

        if (f->name.len == 0) {
            pc->err_pos = pc->pos - 1;
            return NGX_NATS_JSON_ERR_SYNTAX;
        }

        psave = pc->pos;

        rc = _skipSpaces(pc);
        if (rc != 0) {
            pc->err_pos = psave;
            return NGX_NATS_JSON_ERR_EOF;
        }

        c = t[pc->pos++];

        if (c != ':') {
            pc->err_pos = pc->pos - 1;
            return NGX_NATS_JSON_ERR_SYNTAX;
        }

        rc = ngx_nats_json_parse_value(pc, &f->value);
        if (rc != 0) {
            return rc;
        }

        psave = pc->pos;

        rc = _skipSpaces(pc);
        if (rc != 0) {
            pc->err_pos = psave;
            return NGX_NATS_JSON_ERR_EOF;
        }

        psave = pc->pos;

        c = t[pc->pos++];
        if (c == '}')
            return 0;

        if (c == ',') {
            need_field = 1;
            continue;
        }

        pc->err_pos = psave;
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    pc->err_pos = psave;
    return NGX_NATS_JSON_ERR_EOF;
}


static ngx_int_t
ngx_nats_json_parse_array(ngx_nats_json_parse_ctx_t *pc,
                ngx_nats_json_array_t *a)
{
    /* pc->pos is right after '[' */

    ngx_nats_json_value_t  *v;
    u_char                 *t;
    ngx_int_t               rc;
    size_t                  psave;
    u_char                  c, need_field;

    t = pc->text;
    pc->istart = pc->pos - 1;

    psave      = 0;
    need_field = 0;

    while(pc->pos < pc->len) {

        psave = pc->pos;
        rc = _skipSpaces(pc);
        if (rc != 0) {
            pc->err_pos = psave;
            return NGX_NATS_JSON_ERR_EOF;
        }

        pc->istart = pc->pos;
        c = t[pc->pos];

        if (c == ']') {
            if (need_field) {
                pc->err_pos = pc->pos;
                return NGX_NATS_JSON_ERR_SYNTAX;
            }
            pc->pos++;
            return 0;
        }

        v = (ngx_nats_json_value_t *) ngx_array_push(a->values);
        if (v == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }
        ngx_memzero(v, sizeof(ngx_nats_json_value_t));

        rc = ngx_nats_json_parse_value(pc, v);
        if (rc != 0) {
            return rc;
        }

        psave = pc->pos;

        rc = _skipSpaces(pc);
        if (rc != 0) {
            pc->err_pos = psave;
            return NGX_NATS_JSON_ERR_EOF;
        }

        psave = pc->pos;

        c = t[pc->pos++];
        if (c == ']')
            return 0;

        if (c == ',') {
            need_field = 1;
            continue;
        }

        pc->err_pos = psave;
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    pc->err_pos = psave;
    return NGX_NATS_JSON_ERR_EOF;
}

/*
 * Called only with n=2 or n=1 maybe, never too large so 2*cap
 * is always sufficient expansion because we start with 1024.
 */
static ngx_int_t
ngx_nats_json_ensure_str(ngx_nats_json_parse_ctx_t *pc, size_t n)
{
    u_char *ns;
    size_t  sz = pc->str_cap - pc->str_pos; /* available bytes */

    if (sz < n) {

        /*
         * Limit string length, if single string value wants to be
         * more than 1MB then something is wrong.
         */
        if (pc->str_cap >= (size_t)(1024*1024)) {
            /* TODO: log JSON string value too large */
            return NGX_NATS_JSON_ERR_ERROR;
        }

        sz = pc->str_cap * 2;

        ns = ngx_pnalloc(pc->pool, sz);
        if (ns == NULL) {
            /* ngx_pnalloc did log out of memory... */
            return NGX_NATS_JSON_ERR_ERROR;
        }

        ngx_memcpy(ns, pc->str, pc->str_pos);

        ngx_pfree(pc->pool, pc->str);

        pc->str     = ns;
        pc->str_cap = sz;
    }

    return 0;
}


static ngx_int_t
ngx_nats_json_parse_string(ngx_nats_json_parse_ctx_t *pc,
                ngx_str_t *s, u_char q)
{
    /* pc->pos is right after the first quote (I allow ' or ") */

    u_char     *p;
    u_char     *t;
    size_t      n, max, nout, mspos;
    u_char      quote_found = 0;
    u_char      c;

    t = pc->text;
    pc->istart = pc->pos - 1;

    n     = pc->pos;
    max   = pc->len;
    p     = t + pc->pos;     /* after " */
    mspos = pc->str_cap - 2;

    nout  = 0;

    while(n < max) {

        pc->str_pos = nout;

        if (pc->str_pos >= mspos) {
            if (ngx_nats_json_ensure_str(pc, 2) != 0) {
                return NGX_NATS_JSON_ERR_ERROR;
            }
            mspos = pc->str_cap - 2;
        }

        c = *p++;
        n++;

        if (c == '\\') {

            if (n >= max) {
                pc->err_pos = pc->istart;
                return NGX_NATS_JSON_ERR_EOF;
            }

            c = *p++;
            n++;

            switch (c) {

                case 'b':   pc->str[nout++] = '\b';   break;
                case 't':   pc->str[nout++] = '\t';   break;
                case 'n':   pc->str[nout++] = '\n';   break;
                case 'f':   pc->str[nout++] = '\f';   break;
                case 'r':   pc->str[nout++] = '\r';   break;

                case '\"':
                case '\'':
                case '\\':
                case '/':   pc->str[nout++] = c;      break;

                case 'u':
                    pc->err_pos = n - 2;
                    return NGX_NATS_JSON_ERR_NOT_SUPPORTED;

                default:
                    pc->err_pos = n - 2;
                    return NGX_NATS_JSON_ERR_SYNTAX;
            }

            continue;
        }

        if (c == q)
        {
            quote_found = 1;
            break;
        }

        pc->str[nout++] = c;
    }

    pc->pos = n;

    if (quote_found == 0) {
        pc->err_pos = pc->istart;
        return NGX_NATS_JSON_ERR_EOF;
    }

    /* +1 for trailing 0, "nout" does not count it. */
    s->data = ngx_pnalloc(pc->pool, nout + 1);
    if (s->data == NULL) {
        return NGX_NATS_JSON_ERR_ERROR;
    }
    if (nout > 0) {
        ngx_memcpy(s->data, pc->str, nout);
    }
    s->data[nout] = 0;
    s->len = nout;

    return 0;
}


static ngx_int_t
ngx_nats_json_parse_hex(ngx_nats_json_parse_ctx_t *pc,
                ngx_nats_json_value_t *v)
{
    /* pc->istart is at the start of "0X...." text, pc->pos is after value */

    size_t      ilen = pc->pos - pc->istart;
    uint64_t    u64;
    u_char     *p;
    u_char     *end;
    u_char      c;

    pc->err_pos = pc->istart;

    /* skip "0x" that was already found */

    p       = pc->text + pc->istart + 2;
    end     = pc->text + pc->pos;
    ilen   -= 2;

    if (ilen > 16) {
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    u64 = 0;

    while(p < end) {

        c = *p++;

        if (c >= '0' && c <= '9') {
            u64 = (u64 << 4) + (c-'0');
        }
        else if (c >= 'a' && c <= 'f') {
            u64 = (u64 << 4) + (10 + (c-'a'));
        }
        else if (c >= 'A' && c <= 'F') {
            u64 = (u64 << 4) + (10 + (c-'A'));
        }
        else {
            pc->err_pos = pc->istart + (size_t)(p - pc->istart);
            return NGX_NATS_JSON_ERR_SYNTAX;
        }
    }

    v->type       = NGX_NATS_JSON_INTEGER;
    v->value.vint = (int64_t)u64;

    return 0;
}


static ngx_int_t
ngx_nats_json_parse_int(ngx_nats_json_parse_ctx_t *pc,
                ngx_nats_json_value_t *v)
{
    /* pc->istart is at the start of number text, pc->pos is right after */

    size_t      ilen;
    uint64_t    u64;
    u_char     *p;
    u_char     *end;
    u_char      neg = 0;
    u_char      c;

    /*
     * pre-set this, if we return OK then it doesn't matter,
     * in all other cases we'll point to start of the number.
     */
    pc->err_pos = pc->istart;

    ilen = pc->pos - pc->istart;
    p    = pc->text + pc->istart;
    end  = pc->text + pc->pos;

    if ((*p) == '-')
    {
        neg = 1;
        p++;
        ilen--;
    }

    if (ilen <= 0) {
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    u64 = 0;

    while(p < end) {
        c = *p++;

        if (c < '0' || c > '9') {
            return NGX_NATS_JSON_ERR_SYNTAX;
        }

        /* check can still multiply by 10 */
        if (u64 > 922337203685477580LL) {
            return NGX_NATS_JSON_ERR_SYNTAX;
        }

        u64 = (u64 * 10) + (c - '0');
    }

    v->type = NGX_NATS_JSON_INTEGER;

    /* check range */
    if (neg) {
        if ((u64 - 1) > 9223372036854775807LL) {
            return NGX_NATS_JSON_ERR_SYNTAX;
        }
        v->value.vint = -((int64_t) u64);
    }
    else {
        if (u64 > 9223372036854775807LL) {
            return NGX_NATS_JSON_ERR_SYNTAX;
        }
        v->value.vint = (int64_t) u64;
    }

    return 0;
}


static ngx_int_t
ngx_nats_json_parse_double(ngx_nats_json_parse_ctx_t *pc,
                ngx_nats_json_value_t *v)
{
    /* pc->istart is at the start of number text, pc->pos is right after */

    double  d;
    u_char *p;
    u_char *endptr = NULL;
    size_t  ilen = pc->pos - pc->istart;
    u_char  temp[32];

    if (ilen > 31) {
        pc->err_pos = pc->istart;
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    p = &temp[0];

    ngx_memcpy(p, pc->text + pc->istart, ilen);
    p[ilen] = 0;

    d = strtod((char *)p, (char **)&endptr);

    if (endptr != (p+ilen)) {
        pc->err_pos = pc->istart + (size_t)(endptr - p);
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    v->type = NGX_NATS_JSON_DOUBLE;
    v->value.vdec = d;
    return 0;
}


static ngx_int_t
ngx_nats_json_parse_value(ngx_nats_json_parse_ctx_t *pc,
                ngx_nats_json_value_t *v)
{
    ngx_int_t   rc;
    u_char      c, c2;
    size_t      ilen, n;
    u_char     *p;
    u_char     *t;

    t = pc->text;

    rc = _skipSpaces(pc);
    if (rc != 0) {
        return rc;
    }

    c = t[pc->pos];

    if (c == '{') {

        pc->pos++;

        v->type = NGX_NATS_JSON_OBJECT;
        v->value.vobj = ngx_pcalloc(pc->pool, sizeof(ngx_nats_json_object_t));
        if (v->value.vobj == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }
        v->value.vobj->fields = ngx_array_create(pc->pool, 8,
                        sizeof(ngx_nats_json_field_t));
        if (v->value.vobj->fields == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }

        rc = ngx_nats_json_parse_object(pc, v->value.vobj);
        return rc;
    }

    if (c == '[') {

        pc->pos++;

        v->type = NGX_NATS_JSON_ARRAY;
        v->value.varr = ngx_pcalloc(pc->pool, sizeof(ngx_nats_json_array_t));
        if (v->value.varr == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }

        v->value.varr->values = ngx_array_create(pc->pool, 8,
                        sizeof(ngx_nats_json_value_t));
        if (v->value.varr->values == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }

        return ngx_nats_json_parse_array(pc, v->value.varr);
    }

    if (c == '\'' || c == '\"') {
        v->type = NGX_NATS_JSON_STRING;
        pc->pos++;

        v->value.vstr = ngx_pcalloc(pc->pool, sizeof(ngx_str_t));
        if (v->value.vstr == NULL) {
            return NGX_NATS_JSON_ERR_ERROR;
        }

        return ngx_nats_json_parse_string(pc, (ngx_str_t *)v->value.vstr, c);
    }

    /* Primitive value -- null, boolean, integer, double. */

    pc->istart = pc->pos;

    while(pc->pos < pc->len) {

        c = t[pc->pos];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == ',' || c == ':' ||
            c == '[' || c == ']' || c == '{' || c == '}') {
            break;
        }

        pc->pos++;
    }

    ilen = pc->pos - pc->istart;                /* length of item text */
    p    = t + pc->istart;

    /* TODO: I don't support NaN for now, should I? */

    if (ilen == 4) {

        if (ngx_strncasecmp(p, (u_char *)"null", 4) == 0) {
            v->type = NGX_NATS_JSON_NULL;
            return 0;
        }

        if (ngx_strncasecmp(p, (u_char *)"true", 4) == 0) {
            v->type = NGX_NATS_JSON_BOOLEAN;
            v->value.vint = 1;
            return 0;
        }
    }
    else if (ilen == 5 &&
             (ngx_strncasecmp(p, (u_char *)"false", 5)) == 0) {
        v->type = NGX_NATS_JSON_BOOLEAN;
        v->value.vint = 0;
        return 0;
    }

    /*
     * Now the only option it's a number -- integer or double.
     * Notice pc->pos is AFTER the number, pc->istart is start of the number.
     */

    c = t[pc->istart];

    /* Check first character is a valid number start. */
    if ((c < '0' || c > '9') && c != '.' && c != '+' && c != '-') {
        pc->err_pos = pc->istart;
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    if (c == '0') {

        if (ilen == 1) {
            v->type = NGX_NATS_JSON_INTEGER;
            v->value.vint = 0;
            return 0;
        }

        if (ilen > 2 && (t[pc->istart+1] == 'x' || t[pc->istart+1] == 'X')) {
            v->type = NGX_NATS_JSON_INTEGER;
            return ngx_nats_json_parse_hex(pc, v);
        }
    }

    for (n = pc->istart; n < pc->pos; n++) {
        c2 = t[n];
        if (c2 == '.' || c2 == 'e' || c2 == 'E') {
            v->type = NGX_NATS_JSON_DOUBLE;
            return ngx_nats_json_parse_double(pc, v);
        }
    }

    if (c == '0') {
        pc->err_pos = pc->istart;
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    v->type = NGX_NATS_JSON_INTEGER;
    return ngx_nats_json_parse_int(pc, v);
}


/*
 * Public use. All above is private (static) only.
 *
 * Pool must be reset before calling this! Can't do here because
 * "json" may be allocated in that same pool.
 */
ngx_int_t
ngx_nats_json_parse(
        ngx_pool_t *pool,
        ngx_str_t *bytes,
        ngx_nats_json_value_t **json)
{
    ngx_nats_json_value_t      *js;
    ngx_nats_json_parse_ctx_t   pc;
    ngx_int_t                   rc;

    if (bytes->len < 1) {
        /* TODO: log JSON error with full string and the error pos */
        return NGX_NATS_JSON_ERR_SYNTAX;
    }

    js = ngx_pcalloc(pool, sizeof(ngx_nats_json_value_t));
    if (js == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&pc, sizeof(ngx_nats_json_parse_ctx_t));

    /* Empty JSON string is not valid, caller shouldn't call then. */

    pc.pool     = pool;
    pc.text     = bytes->data;
    pc.len      = bytes->len;

    pc.str_cap  = 1024;                     /* will grow if need */
    pc.str = ngx_pnalloc(pool, pc.str_cap); /* non-aligned pool alloc */
    if (pc.str == NULL) {
        return NGX_NATS_JSON_ERR_ERROR;
    }

    pc.err_pos = -1;

    rc = ngx_nats_json_parse_value(&pc, js);

    if (rc != 0) {
        /* TODO: log JSON error with full string and the error pos */
        return rc;
    }

    *json = js;
    return pc.pos;  /* count of parsed bytes */
}

/*
 * For testing only.
 */
static void
_json_debug_print_value(ngx_nats_json_value_t *v)
{
    uint64_t                u64;
    ngx_uint_t              n, lim;
    ngx_nats_json_value_t  *v2;
    ngx_nats_json_field_t  *f;
    u_char                  numbuf[28];
    int                     pos;

    switch(v->type)
    {
        case NGX_NATS_JSON_NULL:
            fprintf(stderr,"null");
            return;

        case NGX_NATS_JSON_BOOLEAN:
            fprintf(stderr,"%s",
                (v->value.vint == 0 ? "false" : "true"));
            return;

        case NGX_NATS_JSON_INTEGER:
            if (v->value.vint < 0) {
                fprintf(stderr,"-");
                u64 = (uint64_t) -(v->value.vint);
            } else {
                u64 = (uint64_t) v->value.vint;
            }
            numbuf[sizeof(numbuf)-1] = 0;
            pos = sizeof(numbuf)-1;
            do {
                numbuf[--pos] = u64 % 10 + '0';
            } while (u64 /= 10);
            fprintf(stderr,"%s",numbuf + pos);
            return;

        case NGX_NATS_JSON_DOUBLE:
            fprintf(stderr,"%f",v->value.vdec);
            return;

        /* as-is, do not escape chars, this is debugging only. */
        case NGX_NATS_JSON_STRING:
            fprintf(stderr,"\"%s\"",v->value.vstr->data);
            return;

        case NGX_NATS_JSON_OBJECT:
            fprintf(stderr,"{");
            f = (ngx_nats_json_field_t *) v->value.vobj->fields->elts;
            lim = v->value.vobj->fields->nelts;
            for (n = 0; n < lim; n++) {
                fprintf(stderr,"\"%s\":",f->name.data);
                _json_debug_print_value(&f->value);
                if (n < lim-1) {
                    fprintf(stderr,",");
                }
                f++;
            }
            fprintf(stderr,"}");
            return;

        case NGX_NATS_JSON_ARRAY:
            fprintf(stderr,"[");
            v2 = (ngx_nats_json_value_t *) v->value.varr->values->elts;
            lim = v->value.varr->values->nelts;
            for (n = 0; n < lim; n++) {
                _json_debug_print_value(v2);
                if (n < lim-1) {
                    fprintf(stderr,",");
                }
                v2++;
            }
            fprintf(stderr,"]");
            return;

        default:
            fprintf(stderr,"<ERROR: unknown value type %d>",(int)v->type);
            return;
    }

}


void
ngx_nats_json_debug_print(ngx_nats_json_value_t *v)
{
    if (v->type != NGX_NATS_JSON_OBJECT && v->type != NGX_NATS_JSON_ARRAY) {
        fprintf(stderr,
            "<error: top level object is not object nor array>\n");
        return;
    }
    _json_debug_print_value(v);
}




/*
 * Copyright (C) Apcera Inc.
 *
 */

#ifndef _NGX_NATS_JSON_H_INCLUDED_
#define _NGX_NATS_JSON_H_INCLUDED_

#include <ngx_core.h>
#include <ngx_palloc.h>


/*
 * Error codes must be negative.
 */
#define NGX_NATS_JSON_ERR_ERROR         (-1)    /* out of memory etc.       */
#define NGX_NATS_JSON_ERR_SYNTAX        (-2)    /* JSON syntax error        */
#define NGX_NATS_JSON_ERR_EOF           (-3)    /* unexpected end of JSON   */
#define NGX_NATS_JSON_ERR_NOT_SUPPORTED (-4)    /* not supported syntax     */


/*
 * JSON value types.
 */
#define NGX_NATS_JSON_NULL              (1)
#define NGX_NATS_JSON_OBJECT            (2)
#define NGX_NATS_JSON_ARRAY             (3)
#define NGX_NATS_JSON_BOOLEAN           (4)
#define NGX_NATS_JSON_INTEGER           (5)
#define NGX_NATS_JSON_DOUBLE            (6)
#define NGX_NATS_JSON_STRING            (7)


typedef struct {

    /* TODO: use ngx_hash_t when have time to figure it out? */
    
    ngx_array_t    *fields;     /* of ngx_nats_json_field */

} ngx_nats_json_object_t;


typedef struct {

    ngx_array_t    *values;     /* of ngx_nats_json_value */

} ngx_nats_json_array_t;


typedef struct {
    
    ngx_int_t   type;
    
    union {
        ngx_nats_json_object_t *vobj;
        ngx_nats_json_array_t  *varr;
        ngx_str_t              *vstr;
        int64_t                 vint;       /* also keeps boolean */
        double                  vdec;       /* decimal */
    } value;

} ngx_nats_json_value_t;


typedef struct {

    ngx_str_t               name;
    ngx_nats_json_value_t   value;

} ngx_nats_json_field_t;


/* 
 * If returns < 0 then it's an error code.
 * If returns > 0 then parsing was successful and return value is the number
 * of parsed bytes, which may be less than the value of "len" param.
 * We do not allow empty JSON string. Notice this never returns 0 value.
 * This may parse an object, array or a primitive value. The caller
 * must check the type if it only expects an object or array, etc.
 *
 * Parsed objects/arrays/values are allocated in a provided pool.
 * Often the caller resets the pool before calling this so same pool
 * is reused.
 */
ngx_int_t ngx_nats_json_parse(ngx_pool_t *pool, ngx_nats_json_value_t *json,
                    char *s, size_t len);

const u_char * ngx_nats_json_type_name(ngx_int_t type);

/*
 * For testing only.
 */
void ngx_nats_json_debug_print(ngx_nats_json_value_t *v);

#endif /* _NGX_NATS_JSON_H_INCLUDED_ */


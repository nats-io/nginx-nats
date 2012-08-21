/*
 * Copyright (C) Apcera Inc.
 *
 * Based on Nginx source code:
 *              Copyright (C) Igor Sysoev
 *              Copyright (C) Nginx, Inc.
 */

#include "ngx_nats.h"
#include "ngx_nats_comm.h"


/*
 * This file deals with the modules, setup, configuration parsing etc.
 * Working with NATS is in ngx_nats_comm.c.
 */

/*
 * Major TODOs:
 *     - implement publish/subscribe interface.
 *     - process exit (shutdown NATS connection).
 *     - user/password per server.
 *
 * TODOs:
 *     - custom log.
 *     - ssl to NATS (?).
 *     - improve buffers.
 *     - use hash in JSON objects.
 */


/*---------------------------------------------------------------------------
 * Forward declarations of functions for main (root) module.
 *--------------------------------------------------------------------------*/

/* Parses nats{...} configuration block. */
static char * ngx_nats_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/*---------------------------------------------------------------------------
 * Forward declarations of functions for core module.
 *--------------------------------------------------------------------------*/

static void * ngx_nats_core_create_conf(ngx_cycle_t *cycle);
static char * ngx_nats_core_init_conf(ngx_conf_t *cf, void *conf);

static ngx_int_t ngx_nats_core_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_nats_core_init_process(ngx_cycle_t *cycle);
static void ngx_nats_core_exit_process(ngx_cycle_t *cycle);

/*
 * Parse "server host:port" line inside the nats{...} block.
 * May have more than one such line.
 */
static char * ngx_nats_core_server(ngx_conf_t *cf,
                    ngx_command_t *cmd, void *dummy);

/*---------------------------------------------------------------------------
 * Variables
 *--------------------------------------------------------------------------*/

static ngx_uint_t   ngx_nats_max_module;

static ngx_str_t    ngx_nats_core_conf_name = ngx_string("nats_core_conf");

static ngx_int_t    ngx_nats_present = 0;

/*---------------------------------------------------------------------------
 * Main module and command nats{...}
 *--------------------------------------------------------------------------*/

static ngx_command_t    ngx_nats_commands[] = {

    { ngx_string("nats"),       /* configuration section */
      NGX_MAIN_CONF | NGX_CONF_BLOCK | NGX_CONF_NOARGS,
      ngx_nats_block,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_core_module_t  ngx_nats_module_ctx = {
    ngx_string("nats_module"),
    NULL,
    NULL
};

ngx_module_t  ngx_nats_module = {
    NGX_MODULE_V1,
    &ngx_nats_module_ctx,                   /* module context */
    ngx_nats_commands,                      /* module directives */
    NGX_CORE_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


/*---------------------------------------------------------------------------
 * Core module processing commands inside nats{...}.
 *--------------------------------------------------------------------------*/

static ngx_command_t  ngx_nats_core_commands[] = {

    {   ngx_string("server"),
        NGX_NATS_CONF | NGX_CONF_TAKE1,
        ngx_nats_core_server,
        0,
        0,
        NULL },

    {   ngx_string("reconnect"),
        NGX_NATS_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_msec_slot,
        0,
        offsetof(ngx_nats_core_conf_t, reconnect_interval),
        NULL },

    {   ngx_string("ping"),
        NGX_NATS_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_msec_slot,
        0,
        offsetof(ngx_nats_core_conf_t, ping_interval),
        NULL },

    {   ngx_string("user"),
        NGX_NATS_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        0,
        offsetof(ngx_nats_core_conf_t, user),
        NULL },

    {   ngx_string("password"),
        NGX_NATS_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        0,
        offsetof(ngx_nats_core_conf_t, password),
        NULL },

    ngx_null_command
};

static ngx_nats_module_t  ngx_nats_core_module_ctx = {
    ngx_string("nats_core_module"),
    ngx_nats_core_create_conf,
    ngx_nats_core_init_conf
};

ngx_module_t  ngx_nats_core_module = {
    NGX_MODULE_V1,
    &ngx_nats_core_module_ctx,              /* module context */
    ngx_nats_core_commands,                 /* module directives */
    NGX_NATS_MODULE,                        /* module type */
    NULL,                                   /* init master */
    ngx_nats_core_init_module,              /* init module */
    ngx_nats_core_init_process,             /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    ngx_nats_core_exit_process,             /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};

/*==========================================================================
 *
 * Main module functions
 *
 *=========================================================================*/

static char *
ngx_nats_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                   *rv;
    void                 ***ctx;
    ngx_conf_t              save_cf;
    ngx_uint_t              i;
    ngx_nats_module_t      *m;
    ngx_nats_core_conf_t   *nccf;

    ngx_nats_present = 1;

    /*
     * Count the number of the nats modules and set up their indices
     */

    ngx_nats_max_module = 0;
    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_NATS_MODULE) {
            continue;
        }

        ngx_modules[i]->ctx_index = ngx_nats_max_module++;
    }

    /*
     * Setup array of configurations for NATS modules.
     * and call create_conf in NATS modules
     * before we parse inside of nats{...} block.
     */

    ctx = ngx_pcalloc(cf->pool, sizeof(void *));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *ctx = ngx_pcalloc(cf->pool, ngx_nats_max_module * sizeof(void *));
    if (*ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * This is Nginx ways. NGX_MAIN_CONF makes Nginx to pass us
     * a pointer to a pointer I need to store here.
     */

    * (void **) conf = ctx;

    /*
     * Call create_conf in NATS modules before we parse
     * inside of the nats{...} block.
     */

    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_NATS_MODULE) {
            continue;
        }

        m = ngx_modules[i]->ctx;

        if (m->create_conf) {
            (*ctx)[ngx_modules[i]->ctx_index] = m->create_conf(cf->cycle);
            if ((*ctx)[ngx_modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    save_cf = *cf;

    /* parse the nats{...} block */

    cf->ctx         = ctx;
    cf->module_type = NGX_NATS_MODULE;
    cf->cmd_type    = NGX_NATS_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save_cf;

    if (rv != NGX_CONF_OK)
        return rv;

    nccf = (ngx_nats_core_conf_t*) ngx_nats_get_core_conf(cf->cycle->conf_ctx);
    if (nccf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "no servers defined inside nats{...} section");
        return NGX_CONF_ERROR;
    }

    for (i = 0; ngx_modules[i]; i++) {
        if (ngx_modules[i]->type != NGX_NATS_MODULE) {
            continue;
        }

        m = ngx_modules[i]->ctx;

        if (m->init_conf) {
            rv = m->init_conf(cf, (*ctx)[ngx_modules[i]->ctx_index]);
            if (rv != NGX_CONF_OK) {
                return rv;
            }
        }
    }

    return NGX_CONF_OK;
}


/*==========================================================================
 *
 * Core module functions
 *
 *=========================================================================*/

static void *
ngx_nats_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_nats_core_conf_t  *nccf;

    nccf = ngx_pcalloc(cycle->pool, sizeof(ngx_nats_core_conf_t));
    if (nccf == NULL) {
        return NULL;
    }

    nccf->name = &ngx_nats_core_conf_name;
    nccf->pool = cycle->pool;
    nccf->log  = cycle->log;

    nccf->reconnect_interval = NGX_CONF_UNSET_MSEC;
    nccf->ping_interval      = NGX_CONF_UNSET_MSEC;

    return nccf;
}


static void
ngx_nats_conf_init_str(ngx_str_t *s, char *value)
{
    if (s->data == NULL) {
        s->len = ngx_strlen(value);
        s->data = (u_char *) value;
    }
}

static char *
ngx_nats_core_init_conf(ngx_conf_t *cf, void *conf)
{
    ngx_nats_core_conf_t  *nccf = conf;

    /* set default if was not in config */
    ngx_conf_init_msec_value(nccf->reconnect_interval,
                             NGX_NATS_DEFAULT_RECONNECT);
    ngx_conf_init_msec_value(nccf->ping_interval, NGX_NATS_DEFAULT_PING);

    ngx_nats_conf_init_str(&nccf->user, NGX_NATS_DEFAULT_USER);
    ngx_nats_conf_init_str(&nccf->password, "");

    if (nccf->user.len+nccf->password.len > NGX_NATS_MAS_USER_PASS_LEN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "NATS: user name and password are too long");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_nats_core_init_module(ngx_cycle_t *cycle)
{
    ngx_nats_core_conf_t   *nccf;
    ngx_nats_data_t        *nd;
    ngx_log_t              *log;
    ngx_int_t               rc;

    if (ngx_nats_present == 0) {
        /*
         * this means Nginx is built with these modules
         * but nats{...} section is not in config. Just ignore.
         */
        return NGX_OK;
    }

    nccf = (ngx_nats_core_conf_t*) ngx_nats_get_core_conf(cycle->conf_ctx);

    log = nccf->pool->log;

    nd = ngx_pcalloc(nccf->pool, sizeof(ngx_nats_data_t));
    if (nd == NULL) {
        return NGX_ERROR;
    }

    nd->nc_pool = ngx_create_pool(NGX_NATS_CONN_POOL_SIZE, log);
    if (nd->nc_pool == NULL) {
        return NGX_ERROR;
    }
    nd->nc_pool->log = log;

    /* 
     * buffers are reused because we only have one NATS
     * connection at the time.
     */
    nd->nc_read_buf = ngx_nats_buf_create(nccf->pool,
                                       NGX_NATS_READ_BUF_INIT_SIZE);
    if (nd->nc_read_buf == NULL) {
        return NGX_ERROR;
    }

    nd->nc_write_buf = ngx_nats_buf_create(nccf->pool, 
                                        NGX_NATS_WRITE_BUF_INIT_SIZE);
    if (nd->nc_write_buf == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_array_init(&nd->cd.clients, nccf->pool, 4, 
                                sizeof(ngx_nats_client_t *));
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_array_init(&nd->cd.subs, nccf->pool, 8, 
                                sizeof(ngx_nats_subscription_t*));
    if (rc != NGX_OK) {
        return rc;
    }

    nccf->data      = nd;
    nd->nccf        = nccf;
    nd->log         = nccf->log;        /* use diff log here ? */
    nd->conn_index  = -1;
    nd->curr_index  = -1;
    nd->last_index  = 0;

    return NGX_OK;
}

static ngx_int_t
ngx_nats_core_init_process(ngx_cycle_t *cycle)
{
    ngx_nats_core_conf_t   *nccf;
    ngx_int_t               rc;

    if (ngx_nats_present == 0) {
        return NGX_OK;
    }

    nccf = (ngx_nats_core_conf_t*) ngx_nats_get_core_conf(cycle->conf_ctx);

    nccf->log = cycle->log;

    /*
     * Returns error in case of some hard failure like no memory
     * or similar, not if could not connect to NATS, it'll retry.
     */
    rc = ngx_nats_init(nccf);

    return rc;
}

static void
ngx_nats_core_exit_process(ngx_cycle_t *cycle)
{
    ngx_nats_core_conf_t   *nccf;

    if (ngx_nats_present == 0) {
        return;
    }

    nccf = (ngx_nats_core_conf_t*) ngx_nats_get_core_conf(cycle->conf_ctx);

    ngx_nats_exit(nccf);
}

/*
 * Parse "server host:port" line inside of nats{...} block.
 */
static char *
ngx_nats_core_server(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_nats_core_conf_t   *nccf = conf;
    ngx_nats_server_t      *ns;
    ngx_str_t              *value;
    ngx_url_t               u;

    /* alloc if first "server" line. */
    if (nccf->servers == NULL) {
        nccf->servers = ngx_array_create(nccf->pool, 4,
                                         sizeof(ngx_nats_server_t));
        if (nccf->servers == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    /* ngx_array's push returns a pointer to where to store the new data. */
    ns = ngx_array_push(nccf->servers);
    if (ns == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(ns, sizeof(ngx_nats_server_t));

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.default_port = NGX_NATS_DEFAULT_PORT;

    if (ngx_parse_url(nccf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in NATS server \"%V\"", u.err, &u.url);
        }

        return NGX_CONF_ERROR;
    }

    ns->addrs   = u.addrs;
    ns->naddrs  = u.naddrs;

    ns->url.len  = u.url.len;

    ns->url.data = ngx_pnalloc(nccf->pool, u.url.len + 1);
    if (ns->url.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(ns->url.data, u.url.data, u.url.len + 1);

    return NGX_CONF_OK;
}


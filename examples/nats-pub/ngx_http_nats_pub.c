#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_nats.h>
#include <ngx_nats_comm.h>

// Sample configuration for module
//
//  location / {
//      root   html;
//      index  index.html index.htm;
//
//    # Notify through NATS processing of a request
//    nats_pub requests;
//  }
//
struct ngx_http_nats_pub_data_s {
    ngx_str_t          subject;
    ngx_nats_client_t  client;
};
typedef struct ngx_http_nats_pub_data_s ngx_http_nats_pub_data_t;

typedef struct {
    ngx_flag_t           enable;
    ngx_str_t            subject;
    ngx_http_nats_pub_data_t *ndata;

} ngx_http_nats_pub_loc_conf_t;

static char*     ngx_http_nats_pub(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void*     ngx_http_nats_pub_create_loc_conf(ngx_conf_t *cf);
static char*     ngx_http_nats_pub_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_nats_pub_init(ngx_conf_t *conf);

static ngx_http_module_t  ngx_http_nats_pub_module_ctx = {
    NULL,                              /* preconfiguration              */
    ngx_http_nats_pub_init,            /* postconfiguration             */
    NULL,                              /* create main configuration     */
    NULL,                              /* init main configuration       */
    NULL,                              /* create server configuration   */
    NULL,                              /* merge server configuration    */
    ngx_http_nats_pub_create_loc_conf, /* create location configuration */
    ngx_http_nats_pub_merge_loc_conf   /* merge location configuration  */
};

static ngx_command_t  ngx_http_nats_pub_commands[] = {
    { ngx_string("nats_pub"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_nats_pub,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
    ngx_null_command
};

ngx_module_t  ngx_http_nats_pub_module = {
    NGX_MODULE_V1,
    &ngx_http_nats_pub_module_ctx,  /* module context */
    ngx_http_nats_pub_commands,     /* module directives */
    NGX_HTTP_MODULE,                /* module type  */
    NULL,                           /* init master  */
    NULL,                           /* init module  */
    NULL,                           /* init process */
    NULL,                           /* init thread  */
    NULL,                           /* exit thread  */
    NULL,                           /* exit process */
    NULL,                           /* exit master  */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_nats_pub_handler(ngx_http_request_t *r)
{
    ngx_http_nats_pub_loc_conf_t  *ncf;
    ngx_str_t replyto = ngx_string(" ");

    // Get the module configuration and use it for publishing messages
    ncf = ngx_http_get_module_loc_conf(r, ngx_http_nats_pub_module);
    if(ncf->enable) {
        // Example: notify remote address from clients
        ngx_int_t rc;
        rc = ngx_nats_publish(&ncf->ndata->client,
                              &ncf->subject, &replyto,
                              r->connection->addr_text.data,
                              r->connection->addr_text.len);
        if (rc != NGX_OK) {
            ngx_log_stderr(0, "%s: publishing message with remote address failed:", __func__);
        }
    }

    return NGX_DECLINED;
}

static char *
ngx_http_nats_pub(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *mcf;
    ngx_http_nats_pub_loc_conf_t  *ncf = conf;
    ngx_str_t                *value;

    // Set configured subject on which messages will be published
    value = cf->args->elts;
    ncf->enable = 1;
    ncf->subject = value[1];

    return NGX_CONF_OK;
}

static void *
ngx_http_nats_pub_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_nats_pub_data_t *ndata;
    ngx_http_nats_pub_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_nats_pub_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->enable = NGX_CONF_UNSET;

    conf->ndata = ngx_pcalloc(cf->pool, sizeof(ngx_http_nats_pub_data_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    return conf;
}

static char *
ngx_http_nats_pub_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_int_t rc;
    ngx_http_nats_pub_loc_conf_t *prev = parent;
    ngx_http_nats_pub_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    if(conf->enable) {
        // Use the present connection to NATS for our module,
        // fails if nginx-nats configuration block is not present.
        rc = ngx_nats_add_client(&conf->ndata->client);
        if (rc != NGX_OK) {
            ngx_log_stderr(0, "%s: merge loc conf error add nats client:", __func__);
        }
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_nats_pub_init(ngx_conf_t *conf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    // Set handler which does the publishing to NATS in the log phase
    cmcf = ngx_http_conf_get_module_main_conf(conf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_nats_pub_handler;

    return NGX_OK;
}


/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Maxim Dounin
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <zlib.h>

#define ENABLE_ORIG 0

typedef struct {
    ngx_flag_t           enable;
    ngx_bufs_t           bufs;
    ngx_flag_t           request_body;
} ngx_http_gunzip_conf_t;


typedef struct {
#if ENABLE_ORIG
    ngx_chain_t         *in;
    ngx_chain_t         *free;
    ngx_chain_t         *busy;
    ngx_chain_t         *out;
    ngx_chain_t        **last_out;

    ngx_buf_t           *in_buf;
    ngx_buf_t           *out_buf;
    ngx_int_t            bufs;

    unsigned             started:1;
    unsigned             flush:4;
    unsigned             redo:1;
    unsigned             done:1;
    unsigned             nomem:1;

    z_stream             zstream;
    ngx_http_request_t  *request;
#endif

    /* fields for request decompressing */

    ngx_chain_t         *recv_in;
    ngx_chain_t         *recv_free;
    ngx_chain_t         *recv_busy;
    ngx_chain_t         *recv_out;
    ngx_chain_t        **recv_last_out;

    ngx_buf_t           *recv_in_buf;
    ngx_buf_t           *recv_out_buf;
    ngx_int_t            recv_bufs;

    unsigned             recv_started:1;
    unsigned             recv_flush:4;
    unsigned             recv_redo:1;
    unsigned             recv_done:1;
    unsigned             recv_nomem:1;

    size_t               recv_sum;

    z_stream             recv_zstream;
    ngx_http_request_t  *recv_request;
} ngx_http_gunzip_ctx_t;


#if ENABLE_ORIG
static ngx_int_t ngx_http_gunzip_filter_inflate_start(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx);
static ngx_int_t ngx_http_gunzip_filter_add_data(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx);
static ngx_int_t ngx_http_gunzip_filter_get_buf(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx);
static ngx_int_t ngx_http_gunzip_filter_inflate(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx);
static ngx_int_t ngx_http_gunzip_filter_inflate_end(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx);
#endif

#if ENABLE_ORIG
static void *ngx_http_gunzip_filter_alloc(void *opaque, u_int items,
    u_int size);
static void ngx_http_gunzip_filter_free(void *opaque, void *address);
#endif

static ngx_int_t ngx_http_gunzip_filter_init(ngx_conf_t *cf);
static void *ngx_http_gunzip_create_conf(ngx_conf_t *cf);
static char *ngx_http_gunzip_merge_conf(ngx_conf_t *cf,
    void *parent, void *child);


static ngx_command_t  ngx_http_gunzip_filter_commands[] = {

    { ngx_string("gunzip"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_gunzip_conf_t, enable),
      NULL },

    { ngx_string("gunzip_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_gunzip_conf_t, bufs),
      NULL },

    { ngx_string("gunzip_request_body"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_gunzip_conf_t, request_body),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_gunzip_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_gunzip_filter_init,           /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_gunzip_create_conf,           /* create location configuration */
    ngx_http_gunzip_merge_conf             /* merge location configuration */
};


ngx_module_t  ngx_http_gunzip_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_gunzip_filter_module_ctx,    /* module context */
    ngx_http_gunzip_filter_commands,       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


#if ENABLE_ORIG
static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;
#endif
static ngx_http_request_body_filter_pt   ngx_http_next_request_body_filter;


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_header_filter(ngx_http_request_t *r)
{
    ngx_http_gunzip_ctx_t   *ctx;
    ngx_http_gunzip_conf_t  *conf;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_gunzip_filter_module);

    /* TODO support multiple content-codings */
    /* TODO always gunzip - due to configuration or module request */
    /* TODO ignore content encoding? */

    if (!conf->enable
        || r->headers_out.content_encoding == NULL
        || r->headers_out.content_encoding->value.len != 4
        || ngx_strncasecmp(r->headers_out.content_encoding->value.data,
                           (u_char *) "gzip", 4) != 0)
    {
        return ngx_http_next_header_filter(r);
    }

    r->gzip_vary = 1;

    if (!r->gzip_tested) {
        if (ngx_http_gzip_ok(r) == NGX_OK) {
            return ngx_http_next_header_filter(r);
        }

    } else if (r->gzip_ok) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_gunzip_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_gunzip_filter_module);

    ctx->request = r;

    r->filter_need_in_memory = 1;

    r->headers_out.content_encoding->hash = 0;
    r->headers_out.content_encoding = NULL;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}
#endif


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    int                     rc;
    ngx_uint_t              flush;
    ngx_chain_t            *cl;
    ngx_http_gunzip_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_gunzip_filter_module);

    if (ctx == NULL || ctx->done || ctx->recv_done) {
        return ngx_http_next_body_filter(r, in);
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http gunzip filter");

    if (!ctx->started) {
        if (ngx_http_gunzip_filter_inflate_start(r, ctx) != NGX_OK) {
            goto failed;
        }
    }

    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
            goto failed;
        }
    }

    if (ctx->nomem) {

        /* flush busy buffers */

        if (ngx_http_next_body_filter(r, NULL) == NGX_ERROR) {
            goto failed;
        }

        cl = NULL;

        ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &cl,
                                (ngx_buf_tag_t) &ngx_http_gunzip_filter_module);
        ctx->nomem = 0;
        flush = 0;

    } else {
        flush = ctx->busy ? 1 : 0;
    }

    for ( ;; ) {

        /* cycle while we can write to a client */

        for ( ;; ) {

            /* cycle while there is data to feed zlib and ... */

            rc = ngx_http_gunzip_filter_add_data(r, ctx);

            if (rc == NGX_DECLINED) {
                break;
            }

            if (rc == NGX_AGAIN) {
                continue;
            }


            /* ... there are buffers to write zlib output */

            rc = ngx_http_gunzip_filter_get_buf(r, ctx);

            if (rc == NGX_DECLINED) {
                break;
            }

            if (rc == NGX_ERROR) {
                goto failed;
            }

            rc = ngx_http_gunzip_filter_inflate(r, ctx);

            if (rc == NGX_OK) {
                break;
            }

            if (rc == NGX_ERROR) {
                goto failed;
            }

            /* rc == NGX_AGAIN */
        }

        if (ctx->out == NULL && !flush) {
            return ctx->busy ? NGX_AGAIN : NGX_OK;
        }

        rc = ngx_http_next_body_filter(r, ctx->out);

        if (rc == NGX_ERROR) {
            goto failed;
        }

        ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &ctx->out,
                                (ngx_buf_tag_t) &ngx_http_gunzip_filter_module);
        ctx->last_out = &ctx->out;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "gunzip out: %p", ctx->out);

        ctx->nomem = 0;
        flush = 0;

        if (ctx->done) {
            return rc;
        }
    }

    /* unreachable */

failed:

    ctx->done = 1;

    return NGX_ERROR;
}
#endif


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_filter_inflate_start(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int  rc;

    ctx->zstream.next_in = Z_NULL;
    ctx->zstream.avail_in = 0;

    ctx->zstream.zalloc = ngx_http_gunzip_filter_alloc;
    ctx->zstream.zfree = ngx_http_gunzip_filter_free;
    ctx->zstream.opaque = ctx;

    /* windowBits +16 to decode gzip, zlib 1.2.0.4+ */
    rc = inflateInit2(&ctx->zstream, MAX_WBITS + 16);

    if (rc != Z_OK) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "inflateInit2() failed: %d", rc);
        return NGX_ERROR;
    }

    ctx->started = 1;

    ctx->last_out = &ctx->out;
    ctx->flush = Z_NO_FLUSH;

    return NGX_OK;
}
#endif


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_filter_add_data(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    if (ctx->zstream.avail_in || ctx->flush != Z_NO_FLUSH || ctx->redo) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "gunzip in: %p", ctx->in);

    if (ctx->in == NULL) {
        return NGX_DECLINED;
    }

    ctx->in_buf = ctx->in->buf;
    ctx->in = ctx->in->next;

    ctx->zstream.next_in = ctx->in_buf->pos;
    ctx->zstream.avail_in = ctx->in_buf->last - ctx->in_buf->pos;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "gunzip in_buf:%p ni:%p ai:%ud",
                   ctx->in_buf,
                   ctx->zstream.next_in, ctx->zstream.avail_in);

    if (ctx->in_buf->last_buf || ctx->in_buf->last_in_chain) {
        ctx->flush = Z_FINISH;

    } else if (ctx->in_buf->flush) {
        ctx->flush = Z_SYNC_FLUSH;

    } else if (ctx->zstream.avail_in == 0) {
        /* ctx->flush == Z_NO_FLUSH */
        return NGX_AGAIN;
    }

    return NGX_OK;
}
#endif

#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_filter_get_buf(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    ngx_http_gunzip_conf_t  *conf;

    if (ctx->zstream.avail_out) {
        return NGX_OK;
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_gunzip_filter_module);

    if (ctx->free) {
        ctx->out_buf = ctx->free->buf;
        ctx->free = ctx->free->next;

        ctx->out_buf->flush = 0;

    } else if (ctx->bufs < conf->bufs.num) {

        ctx->out_buf = ngx_create_temp_buf(r->pool, conf->bufs.size);
        if (ctx->out_buf == NULL) {
            return NGX_ERROR;
        }

        ctx->out_buf->tag = (ngx_buf_tag_t) &ngx_http_gunzip_filter_module;
        ctx->out_buf->recycled = 1;
        ctx->bufs++;

    } else {
        ctx->nomem = 1;
        return NGX_DECLINED;
    }

    ctx->zstream.next_out = ctx->out_buf->pos;
    ctx->zstream.avail_out = conf->bufs.size;

    return NGX_OK;
}
#endif


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_filter_inflate(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int           rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "inflate in: ni:%p no:%p ai:%ud ao:%ud fl:%d redo:%d",
                   ctx->zstream.next_in, ctx->zstream.next_out,
                   ctx->zstream.avail_in, ctx->zstream.avail_out,
                   ctx->flush, ctx->redo);

    rc = inflate(&ctx->zstream, ctx->flush);

    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "inflate() failed: %d, %d", ctx->flush, rc);
        return NGX_ERROR;
    }

    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "inflate out: ni:%p no:%p ai:%ud ao:%ud rc:%d",
                   ctx->zstream.next_in, ctx->zstream.next_out,
                   ctx->zstream.avail_in, ctx->zstream.avail_out,
                   rc);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "gunzip in_buf:%p pos:%p",
                   ctx->in_buf, ctx->in_buf->pos);

    if (ctx->zstream.next_in) {
        ctx->in_buf->pos = ctx->zstream.next_in;

        if (ctx->zstream.avail_in == 0) {
            ctx->zstream.next_in = NULL;
        }
    }

    ctx->out_buf->last = ctx->zstream.next_out;

    if (ctx->zstream.avail_out == 0) {

        /* zlib wants to output some more data */

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = ctx->out_buf;
        cl->next = NULL;
        *ctx->last_out = cl;
        ctx->last_out = &cl->next;

        ctx->redo = 1;

        return NGX_AGAIN;
    }

    ctx->redo = 0;

    if (ctx->flush == Z_SYNC_FLUSH) {

        ctx->flush = Z_NO_FLUSH;

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        b = ctx->out_buf;

        if (ngx_buf_size(b) == 0) {

            b = ngx_calloc_buf(ctx->request->pool);
            if (b == NULL) {
                return NGX_ERROR;
            }

        } else {
            ctx->zstream.avail_out = 0;
        }

        b->flush = 1;

        cl->buf = b;
        cl->next = NULL;
        *ctx->last_out = cl;
        ctx->last_out = &cl->next;

        return NGX_OK;
    }

    if (ctx->flush == Z_FINISH && ctx->zstream.avail_in == 0) {

        if (rc != Z_STREAM_END) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "inflate() returned %d on response end", rc);
            return NGX_ERROR;
        }

        if (ngx_http_gunzip_filter_inflate_end(r, ctx) != NGX_OK) {
            return NGX_ERROR;
        }

        return NGX_OK;
    }

    if (rc == Z_STREAM_END && ctx->zstream.avail_in > 0) {

        rc = inflateReset(&ctx->zstream);

        if (rc != Z_OK) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "inflateReset() failed: %d", rc);
            return NGX_ERROR;
        }

        ctx->redo = 1;

        return NGX_AGAIN;
    }

    if (ctx->in == NULL) {

        b = ctx->out_buf;

        if (ngx_buf_size(b) == 0) {
            return NGX_OK;
        }

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        ctx->zstream.avail_out = 0;

        cl->buf = b;
        cl->next = NULL;
        *ctx->last_out = cl;
        ctx->last_out = &cl->next;

        return NGX_OK;
    }

    return NGX_AGAIN;
}
#endif


#if ENABLE_ORIG
static ngx_int_t
ngx_http_gunzip_filter_inflate_end(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int           rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "gunzip inflate end");

    rc = inflateEnd(&ctx->zstream);

    if (rc != Z_OK) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "inflateEnd() failed: %d", rc);
        return NGX_ERROR;
    }

    b = ctx->out_buf;

    if (ngx_buf_size(b) == 0) {

        b = ngx_calloc_buf(ctx->request->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;
    *ctx->last_out = cl;
    ctx->last_out = &cl->next;

    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;
    b->sync = 1;

    ctx->done = 1;

    return NGX_OK;
}
#endif


#if ENABLE_ORIG
static void *
ngx_http_gunzip_filter_alloc(void *opaque, u_int items, u_int size)
{
    ngx_http_gunzip_ctx_t *ctx = opaque;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "gunzip alloc: n:%ud s:%ud",
                   items, size);

    return ngx_palloc(ctx->request->pool, items * size);
}
#endif


#if ENABLE_ORIG
static void
ngx_http_gunzip_filter_free(void *opaque, void *address)
{
#if 0
    ngx_http_gunzip_ctx_t *ctx = opaque;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "gunzip free: %p", address);
#endif
}
#endif


static void *
ngx_http_gunzip_create_conf(ngx_conf_t *cf)
{
    ngx_http_gunzip_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_gunzip_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->bufs.num = 0;
     */

    conf->enable = NGX_CONF_UNSET;

    conf->request_body = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_gunzip_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_gunzip_conf_t *prev = parent;
    ngx_http_gunzip_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs,
                              (128 * 1024) / ngx_pagesize, ngx_pagesize);

    ngx_conf_merge_value(conf->request_body,
                         prev->request_body, 0);

    return NGX_CONF_OK;
}

static void *
ngx_http_gunzip_request_filter_alloc(void *opaque, u_int items, u_int size)
{
    ngx_http_gunzip_ctx_t *ctx = opaque;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ctx->recv_request->connection->log, 0,
                   "[gunrecv] gunzip alloc: n:%ud s:%ud",
                   items, size);

    return ngx_palloc(ctx->recv_request->pool, items * size);
}


static void
ngx_http_gunzip_request_filter_free(void *opaque, void *address)
{
#if 0
    ngx_http_gunzip_ctx_t *ctx = opaque;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                   "[gunrecv] gunzip free: %p", address);
#endif
}

static ngx_int_t
ngx_http_gunzip_request_filter_inflate_start(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int  rc;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] inflate start");

    ctx->recv_request = r;

    ctx->recv_zstream.next_in = Z_NULL;
    ctx->recv_zstream.avail_in = 0;

    ctx->recv_zstream.zalloc = ngx_http_gunzip_request_filter_alloc;
    ctx->recv_zstream.zfree = ngx_http_gunzip_request_filter_free;
    ctx->recv_zstream.opaque = ctx;

    /* windowBits +16 to decode gzip, zlib 1.2.0.4+ */
    rc = inflateInit2(&ctx->recv_zstream, MAX_WBITS + 16);

    if (rc != Z_OK) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "inflateInit2() failed: %d", rc);
        return NGX_ERROR;
    }

    ctx->recv_started = 1;

    ctx->recv_last_out = &ctx->recv_out;
    ctx->recv_flush = Z_NO_FLUSH;

    return NGX_OK;
}

static ngx_int_t
ngx_http_gunzip_request_filter_inflate_end(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int           rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] gunzip inflate end");

    rc = inflateEnd(&ctx->recv_zstream);

    if (rc != Z_OK) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "[gunrecv] inflateEnd() failed: %d", rc);
        return NGX_ERROR;
    }

    b = ctx->recv_out_buf;

    // update content_length_n
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] recv_sum=%d", ctx->recv_sum);
    r->headers_in.content_length_n = ctx->recv_sum;

    if (ngx_buf_size(b) == 0) {

        b = ngx_calloc_buf(ctx->recv_request->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;
    *ctx->recv_last_out = cl;
    ctx->recv_last_out = &cl->next;

    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;
    b->sync = 1;

    ctx->recv_done = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_gunzip_request_filter_add_data(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    if (ctx->recv_zstream.avail_in || ctx->recv_flush != Z_NO_FLUSH || ctx->recv_redo) {
        return NGX_OK;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] in: %p (%d)", ctx->recv_in, ctx->recv_in != NULL ? ngx_buf_size(ctx->recv_in->buf): -1);

    if (ctx->recv_in == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] add_data case#5");
        return NGX_DECLINED;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] in#2: size=%d next=%p", ngx_buf_size(ctx->recv_in->buf), ctx->recv_in->next);

    ctx->recv_in_buf = ctx->recv_in->buf;
    ctx->recv_in = ctx->recv_in->next;

    ctx->recv_zstream.next_in = ctx->recv_in_buf->pos;
    ctx->recv_zstream.avail_in = ctx->recv_in_buf->last - ctx->recv_in_buf->pos;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] in_buf:%p ni:%p ai:%ud",
                   ctx->recv_in_buf,
                   ctx->recv_zstream.next_in, ctx->recv_zstream.avail_in);

    if (ctx->recv_in_buf->last_buf || ctx->recv_in_buf->last_in_chain) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] add_data case#1");
        ctx->recv_flush = Z_FINISH;

    } else if (ctx->recv_in_buf->flush) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] add_data case#2");
        ctx->recv_flush = Z_SYNC_FLUSH;

    } else if (ctx->recv_zstream.avail_in == 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] add_data case#3");
        return NGX_AGAIN;
    } else {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] add_data case#0");
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_gunzip_request_filter_get_buf(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    ngx_http_gunzip_conf_t  *conf;

    if (ctx->recv_zstream.avail_out) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] get_buf: case#0");
        return NGX_OK;
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_gunzip_filter_module);

    if (ctx->recv_free) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] get_buf: case#1");
        ctx->recv_out_buf = ctx->recv_free->buf;
        ctx->recv_free = ctx->recv_free->next;

        ctx->recv_out_buf->flush = 0;

    } else if (ctx->recv_bufs < conf->bufs.num) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] get_buf: case#2");

        ctx->recv_out_buf = ngx_create_temp_buf(r->pool, conf->bufs.size);
        if (ctx->recv_out_buf == NULL) {
            return NGX_ERROR;
        }

        ctx->recv_out_buf->tag = (ngx_buf_tag_t) &ngx_http_gunzip_filter_module;
        ctx->recv_out_buf->recycled = 1;
        ctx->recv_bufs++;

    } else {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] get_buf: case#3");
        ctx->recv_nomem = 1;
        return NGX_DECLINED;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] get_buf: case#4");
    ctx->recv_zstream.next_out = ctx->recv_out_buf->pos;
    ctx->recv_zstream.avail_out = conf->bufs.size;

    return NGX_OK;
}

static ngx_int_t
ngx_http_gunzip_request_filter_inflate(ngx_http_request_t *r,
    ngx_http_gunzip_ctx_t *ctx)
{
    int           rc;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;
    size_t        curr;

    curr = ctx->recv_zstream.avail_out;
    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] inflate in: ni:%p no:%p ai:%ud ao:%ud fl:%d redo:%d",
                   ctx->recv_zstream.next_in, ctx->recv_zstream.next_out,
                   ctx->recv_zstream.avail_in, ctx->recv_zstream.avail_out,
                   ctx->recv_flush, ctx->recv_redo);

    rc = inflate(&ctx->recv_zstream, ctx->recv_flush);

    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "[gunrecv] inflate() failed: %d, %d", ctx->recv_flush, rc);
        return NGX_ERROR;
    }

    if (curr > ctx->recv_zstream.avail_out) {
        ctx->recv_sum += curr - ctx->recv_zstream.avail_out;
    }
    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] inflate out: ni:%p no:%p ai:%ud ao:%ud rc:%d",
                   ctx->recv_zstream.next_in, ctx->recv_zstream.next_out,
                   ctx->recv_zstream.avail_in, ctx->recv_zstream.avail_out,
                   rc);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] gunzip in_buf:%p pos:%p",
                   ctx->recv_in_buf, ctx->recv_in_buf->pos);

    if (ctx->recv_zstream.next_in) {
        ctx->recv_in_buf->pos = ctx->recv_zstream.next_in;

        if (ctx->recv_zstream.avail_in == 0) {
            ctx->recv_zstream.next_in = NULL;
        }
    }

    ctx->recv_out_buf->last = ctx->recv_zstream.next_out;

    if (ctx->recv_zstream.avail_out == 0) {

        /* zlib wants to output some more data */

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = ctx->recv_out_buf;
        cl->next = NULL;
        *ctx->recv_last_out = cl;
        ctx->recv_last_out = &cl->next;

        ctx->recv_redo = 1;

        return NGX_AGAIN;
    }

    ctx->recv_redo = 0;

    if (ctx->recv_flush == Z_SYNC_FLUSH) {

        ctx->recv_flush = Z_NO_FLUSH;

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        b = ctx->recv_out_buf;

        if (ngx_buf_size(b) == 0) {

            b = ngx_calloc_buf(ctx->recv_request->pool);
            if (b == NULL) {
                return NGX_ERROR;
            }

        } else {
            ctx->recv_zstream.avail_out = 0;
        }

        b->flush = 1;

        cl->buf = b;
        cl->next = NULL;
        *ctx->recv_last_out = cl;
        ctx->recv_last_out = &cl->next;

        return NGX_OK;
    }

    if (ctx->recv_flush == Z_FINISH && ctx->recv_zstream.avail_in == 0) {

        if (rc != Z_STREAM_END) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "[gunrecv] inflate() returned %d on response end", rc);
            return NGX_ERROR;
        }

        if (ngx_http_gunzip_request_filter_inflate_end(r, ctx) != NGX_OK) {
            return NGX_ERROR;
        }

        return NGX_OK;
    }

    if (rc == Z_STREAM_END && ctx->recv_zstream.avail_in > 0) {

        rc = inflateReset(&ctx->recv_zstream);

        if (rc != Z_OK) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "[gunrecv] inflateReset() failed: %d", rc);
            return NGX_ERROR;
        }

        ctx->recv_redo = 1;

        return NGX_AGAIN;
    }

    if (ctx->recv_in == NULL) {

        b = ctx->recv_out_buf;

        if (ngx_buf_size(b) == 0) {
            return NGX_OK;
        }

        cl = ngx_alloc_chain_link(r->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        ctx->recv_zstream.avail_out = 0;

        cl->buf = b;
        cl->next = NULL;
        *ctx->recv_last_out = cl;
        ctx->recv_last_out = &cl->next;

        return NGX_OK;
    }

    return NGX_AGAIN;
}

static ngx_int_t
ngx_http_gunzip_request_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_gunzip_conf_t *conf;
    ngx_uint_t              i;
    ngx_list_part_t        *part;
    ngx_table_elt_t        *header;
    ngx_int_t               decompress = 0;
    ngx_http_gunzip_ctx_t  *ctx;
    ngx_uint_t              flush;
    ngx_chain_t            *cl;
    int                     rc;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "[gunrecv] filter invoked");
    for (cl = in; cl; cl = cl->next) {
        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, r->connection->log, 0,
                       "[gunrecv] new buf t:%d f:%d %p, pos %p, size: %z "
                       "file: %O, size: %O",
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_gunzip_filter_module);
    if (!conf->request_body) {
        return ngx_http_next_request_body_filter(r, in);
    }

    part = &r->headers_in.headers.part;
    header = part->elts;
    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].key.len == sizeof("Content-Encoding") - 1
            && ngx_strncasecmp(header[i].key.data,
                               (u_char *) "Content-Encoding",
                               sizeof("Content-Encoding") - 1) == 0
            && header[i].value.len == 4
            && ngx_strncasecmp(header[i].value.data,
                               (u_char *) "gzip", 4) == 0)
        {
            decompress = 1;
            break;
        }
    }

    if (!decompress) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "[gunrecv] thru");
        rc = ngx_http_next_request_body_filter(r, in);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] thru next post: rc=%d busy.size=%d", rc, (ctx->recv_busy != NULL ? ngx_buf_size(ctx->recv_busy->buf) : -1));
        return rc;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "[gunrecv] decompress request body");

    ctx = ngx_http_get_module_ctx(r, ngx_http_gunzip_filter_module);
    if (ctx != NULL && ctx->recv_done) {
        return ngx_http_next_request_body_filter(r, in);
    }
    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_gunzip_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }
        ngx_http_set_ctx(r, ctx, ngx_http_gunzip_filter_module);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "[gunrecv] available ctx: busy=%d", ctx->recv_busy != 0);

    if (!ctx->recv_started) {
        if (ngx_http_gunzip_request_filter_inflate_start(r, ctx) != NGX_OK) {
            goto failed;
        }
    }

    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->recv_in, in) != NGX_OK) {
            goto failed;
        }
    }

    if (ctx->recv_nomem) {
        /* flush busy buffers */
        if (ngx_http_next_request_body_filter(r, NULL) == NGX_ERROR) {
            goto failed;
        }
        cl = NULL;
        ngx_chain_update_chains(r->pool, &ctx->recv_free, &ctx->recv_busy, &cl,
                                (ngx_buf_tag_t) &ngx_http_gunzip_filter_module);
        ctx->recv_nomem = 0;
        flush = 0;
    } else {
        flush = ctx->recv_busy ? 1 : 0;
    }

    for ( ;; ) {
        /* cycle while we can write to a client */
        for ( ;; ) {

            /* cycle while there is data to feed zlib and ... */
            rc = ngx_http_gunzip_request_filter_add_data(r, ctx);
            if (rc == NGX_DECLINED) {
                break;
            }
            if (rc == NGX_AGAIN) {
                continue;
            }

            /* ... there are buffers to write zlib output */
            rc = ngx_http_gunzip_request_filter_get_buf(r, ctx);
            if (rc == NGX_DECLINED) {
                break;
            }
            if (rc == NGX_ERROR) {
                goto failed;
            }

            rc = ngx_http_gunzip_request_filter_inflate(r, ctx);
            if (rc == NGX_OK) {
                break;
            }
            if (rc == NGX_ERROR) {
                goto failed;
            }
            /* rc == NGX_AGAIN */
        }

        if (ctx->recv_out == NULL && !flush) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "[gunrecv] no out, no flush: busy=%d nomem=%d", ctx->recv_busy != 0, ctx->recv_nomem);
            return NGX_OK;
            return ctx->recv_busy ? NGX_AGAIN : NGX_OK;
        }


        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] next pre: out.size=%d", (ctx->recv_out != NULL ? ngx_buf_size(ctx->recv_out->buf) : -1));
        rc = ngx_http_next_request_body_filter(r, ctx->recv_out);
        if (rc == NGX_ERROR) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "[gunrecv] error");
            goto failed;
        }
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "[gunrecv] next post: out.size=%d", (ctx->recv_out != NULL ? ngx_buf_size(ctx->recv_out->buf) : -1));

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "[gunrecv] update_chains#2 pre: busy=%d out.size=%d",
                       ctx->recv_busy != 0,
                       (ctx->recv_out != NULL ?
                       ngx_buf_size(ctx->recv_out->buf) : -1));
        ngx_chain_update_chains(r->pool, &ctx->recv_free, &ctx->recv_busy, &ctx->recv_out,
                                (ngx_buf_tag_t) &ngx_http_gunzip_filter_module);
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "[gunrecv] update_chains#2 post: busy=%d", ctx->recv_busy != 0);
        ctx->recv_last_out = &ctx->recv_out;

        ctx->recv_nomem = 0;
        flush = 0;

        if (ctx->recv_done) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "[gunrecv] done: rc=%d", rc);
            return rc;
        }
    }

    /* unreachable */

failed:
    ctx->recv_done = 1;
    return NGX_ERROR;
}

static ngx_int_t
ngx_http_gunzip_filter_init(ngx_conf_t *cf)
{
#if ENABLE_ORIG
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_gunzip_header_filter;
#endif

#if ENABLE_ORIG
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_gunzip_body_filter;
#endif

    ngx_http_next_request_body_filter = ngx_http_top_request_body_filter;
    ngx_http_top_request_body_filter = ngx_http_gunzip_request_body_filter;

    return NGX_OK;
}

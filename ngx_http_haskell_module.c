/*
 * =============================================================================
 *
 *       Filename:  ngx_http_haskell_module.c
 *
 *    Description:  nginx module for inlining haskell code
 *
 *        Version:  1.0
 *        Created:  23.12.2015 12:53:00
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <dlfcn.h>
#include <HsFFI.h>

#define STRLEN(X) sizeof(X) - 1


static const char  haskell_module_handler_prefix[] = "ngx_hs_";

static const char  haskell_module_code_head[] =
"{-# LANGUAGE ForeignFunctionInterface, CPP #-}""\n\n"
"#define NGX_EXPORT_S_S(F) ngx_hs_ ## F = aux_ngx_hs_s_s $ AUX_NGX_S_S $ F; "
"\\\n"
"foreign export ccall ngx_hs_ ## F :: "
"AUX_NGX.CString -> AUX_NGX.CInt -> AUX_NGX.Ptr AUX_NGX.CString -> "
"IO AUX_NGX.CInt\n"
"#define NGX_EXPORT_S_SS(F) ngx_hs_ ## F = aux_ngx_hs_s_ss $ AUX_NGX_S_SS $ F; "
"\\\n"
"foreign export ccall ngx_hs_ ## F :: "
"AUX_NGX.CString -> AUX_NGX.CInt -> AUX_NGX.CString -> AUX_NGX.CInt -> "
"AUX_NGX.Ptr AUX_NGX.CString -> IO AUX_NGX.CInt\n\n"
"module NgxHaskellUserRuntime where\n\n"
"import qualified Foreign.C as AUX_NGX\n"
"import qualified Foreign.Ptr as AUX_NGX\n"
"import qualified Foreign.Storable as AUX_NGX\n"
"import qualified System.IO.Unsafe as AUX_NGX\n";

static const char  haskell_module_code_tail[] =
"\ndata AUX_NGX_SF = AUX_NGX_S_S (String -> String) |\n"
"          AUX_NGX_S_SS (String -> String -> String)\n\n"
"aux_ngx_peekUnsafeCStringLen :: AUX_NGX.CString -> Int -> String\n"
"aux_ngx_peekUnsafeCStringLen x = "
"AUX_NGX.unsafePerformIO . curry AUX_NGX.peekCStringLen x\n"
"aux_ngx_hs_s_s :: "
"AUX_NGX_SF -> AUX_NGX.CString -> AUX_NGX.CInt -> "
"AUX_NGX.Ptr AUX_NGX.CString -> IO AUX_NGX.CInt\n"
"aux_ngx_hs_s_s (AUX_NGX_S_S f) x n p = do\n"
"    (s, l) <- AUX_NGX.newCStringLen $ f $ aux_ngx_peekUnsafeCStringLen x $ "
"fromIntegral n\n"
"    AUX_NGX.poke p s\n"
"    return $ fromIntegral l\n"
"aux_ngx_hs_s_ss :: AUX_NGX_SF -> "
"AUX_NGX.CString -> AUX_NGX.CInt -> AUX_NGX.CString -> AUX_NGX.CInt -> "
"AUX_NGX.Ptr AUX_NGX.CString -> IO AUX_NGX.CInt\n"
"aux_ngx_hs_s_ss (AUX_NGX_S_SS f) x n y m p = do\n"
"    (s, l) <- AUX_NGX.newCStringLen $ f "
"(aux_ngx_peekUnsafeCStringLen x $ fromIntegral n) $ "
"aux_ngx_peekUnsafeCStringLen y $ fromIntegral m\n"
"    AUX_NGX.poke p s\n"
"    return $ fromIntegral l\n";

static const char  haskell_compile_cmd[] =
    "ghc --make -O2 -shared -dynamic -no-hs-main -pgmP cpp"
    " -optl-Wl,-rpath,$(ghc --print-libdir)/rts"
    " -lHSrts-ghc$(ghc --numeric-version) -o ";


typedef HsInt32 (*ngx_http_haskell_handler_s_s)
    (HsPtr, HsInt32, HsPtr);
typedef HsInt32 (*ngx_http_haskell_handler_s_ss)
    (HsPtr, HsInt32, HsPtr, HsInt32, HsPtr);

typedef enum {
    ngx_http_haskell_handler_type_s_s = 0,
    ngx_http_haskell_handler_type_s_ss
} ngx_http_haskell_handler_type_e;


typedef struct {
    ngx_uint_t                        code_loaded;
    ngx_str_t                         ghc_extra_flags;
    ngx_str_t                         lib_path;
    ngx_array_t                       handlers;
    void                             *dl_handle;
    void                            (*hs_init)(int *, char ***);
    void                            (*hs_exit)(void);
    void                            (*hs_add_root)(void (*)(void));
    void                            (*init_HsModule)(void);
} ngx_http_haskell_main_conf_t;


typedef struct {
    ngx_array_t                       code_vars;
} ngx_http_haskell_loc_conf_t;


typedef struct {
    void                             *self;
    ngx_str_t                         name;
} ngx_http_haskell_ref_handler_t;


typedef struct {
    ngx_int_t                         index;
    ngx_str_t                         name;
    ngx_http_haskell_handler_type_e   type;
} ngx_http_haskell_handler_t;


typedef struct {
    ngx_int_t                         index;
    ngx_http_haskell_handler_t        handler;
    ngx_http_complex_value_t          args[2];
} ngx_http_haskell_code_var_data_t;


static char *ngx_http_haskell(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_haskell_write_code(ngx_conf_t *cf, ngx_str_t source_name,
    ngx_str_t fragment);
static char *ngx_http_haskell_compile(ngx_conf_t *cf, void *conf,
    ngx_str_t source_name);
static ngx_int_t ngx_http_haskell_load(ngx_cycle_t *cycle);
static char *ngx_http_haskell_run(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_haskell_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_haskell_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_haskell_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_haskell_init(ngx_cycle_t *cycle);
static void ngx_http_haskell_exit(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_haskell_run_handler(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);


static ngx_command_t  ngx_http_haskell_module_commands[] = {

    { ngx_string("haskell"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE23,
      ngx_http_haskell,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("haskell_run"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE3|NGX_CONF_TAKE4,
      ngx_http_haskell_run,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_haskell_module_ctx = {
    NULL,                                    /* preconfiguration */
    NULL,                                    /* postconfiguration */

    ngx_http_haskell_create_main_conf,       /* create main configuration */
    NULL,                                    /* init main configuration */

    NULL,                                    /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_haskell_create_loc_conf,        /* create location configuration */
    ngx_http_haskell_merge_loc_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_haskell_module = {
    NGX_MODULE_V1,
    &ngx_http_haskell_module_ctx,            /* module context */
    ngx_http_haskell_module_commands,        /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    NULL,                                    /* init module */
    ngx_http_haskell_init,                   /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    ngx_http_haskell_exit,                   /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_http_haskell_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_haskell_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_haskell_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&mcf->handlers, cf->pool, 1,
                       sizeof(ngx_http_haskell_ref_handler_t)) != NGX_OK)
    {
        return NULL;
    }

    return mcf;
}


static void *
ngx_http_haskell_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_haskell_loc_conf_t  *lcf;

    lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_haskell_loc_conf_t));
    if (lcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&lcf->code_vars, cf->pool, 1,
                       sizeof(ngx_http_haskell_code_var_data_t)) != NGX_OK)
    {
        return NULL;
    }

    return lcf;
}


static char *
ngx_http_haskell_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_haskell_loc_conf_t  *prev = parent;
    ngx_http_haskell_loc_conf_t  *conf = child;

    ngx_uint_t                    i;

    for (i = 0; i < prev->code_vars.nelts; i++) {
        ngx_http_haskell_code_var_data_t  *elem;

        elem = ngx_array_push(&conf->code_vars);
        if (elem == NULL) {
            return NGX_CONF_ERROR;
        }

        *elem = ((ngx_http_haskell_code_var_data_t *) prev->code_vars.elts)[i];
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_haskell_init(ngx_cycle_t *cycle)
{
    ngx_http_haskell_main_conf_t  *mcf;

    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_haskell_module);

    if (ngx_http_haskell_load(cycle) != NGX_OK) {
        return NGX_ERROR;
    }

    if (mcf != NULL && mcf->dl_handle != NULL) {
        mcf->hs_init(&ngx_argc, &ngx_argv);
        mcf->hs_add_root(mcf->init_HsModule);
    }

    return NGX_OK;
}


static void
ngx_http_haskell_exit(ngx_cycle_t *cycle)
{
    ngx_http_haskell_main_conf_t  *mcf;

    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_haskell_module);

    if (mcf != NULL && mcf->dl_handle != NULL) {
        mcf->hs_exit();
        dlclose(mcf->dl_handle);
    }
}


static char *
ngx_http_haskell(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_haskell_main_conf_t  *mcf = conf;

    ngx_int_t                      i;
    ngx_str_t                     *value, base_name;
    ngx_file_info_t                lib_info;
    ngx_uint_t                     load = 0, load_without_code = 0;
    ngx_uint_t                     base_name_start = 0;

    if (mcf->code_loaded) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "only one haskell source code block is allowed");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (value[1].len == 7
        && ngx_strncmp(value[1].data, "compile", 7) == 0)
    {
        if (cf->args->nelts < 4) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "directive haskell compile requires 2 parameters");
            return NGX_CONF_ERROR;
        }
    } else if (value[1].len == 4
               && ngx_strncmp(value[1].data, "load", 4) == 0)
    {
        load = 1;
        load_without_code = cf->args->nelts < 4 ? 1 : 0;
    } else if (value[1].len == 15
               && ngx_strncmp(value[1].data, "ghc_extra_flags", 15) == 0)
    {
        if (cf->args->nelts != 3) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "directive haskell ghc_extra_flags requires 1 parameter");
            return NGX_CONF_ERROR;
        }
        if (mcf->ghc_extra_flags.len > 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "directive haskell ghc_extra_flags was already set");
            return NGX_CONF_ERROR;
        }
        mcf->ghc_extra_flags = value[2];
        return NGX_CONF_OK;
    } else
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown haskell directive \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (value[2].len < 3 ||
        ngx_strncmp(value[2].data + value[2].len - 3, ".hs", 3) != 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "haskell source code file must have extension \".hs\"");
        return NGX_CONF_ERROR;
    }
    if (!ngx_path_separator(value[2].data[0])) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "haskell source code file path must be absolute");
        return NGX_CONF_ERROR;
    }
    for (i = value[2].len - 4; i >= 0; i--) {
        if (ngx_path_separator(value[2].data[i])) {
            base_name_start = i;
            break;
        }
    }
    base_name.len = value[2].len - 4 - base_name_start;
    base_name.data = value[2].data + base_name_start;
    if (base_name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "haskell source code file base name is empty");
        return NGX_CONF_ERROR;
    }

    mcf->lib_path.len = value[2].len;
    mcf->lib_path.data = ngx_pnalloc(cf->pool, mcf->lib_path.len + 1);
    if (mcf->lib_path.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(mcf->lib_path.data, value[2].data, value[2].len - 3);
    ngx_memcpy(mcf->lib_path.data + value[2].len - 3, ".so", 3);
    mcf->lib_path.data[value[2].len] = '\0';

    if (load) {
        if (ngx_file_info(mcf->lib_path.data, &lib_info) == NGX_FILE_ERROR) {
            if (load_without_code) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "haskell library cannot be loaded nor compiled");
                return NGX_CONF_ERROR;
            }
            load = 0;
        }
    }

    if (!load) {
        if (ngx_http_haskell_write_code(cf, value[2], value[3])
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }

        if (ngx_http_haskell_compile(cf, conf, value[2])
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    mcf->code_loaded = 1;

    return NGX_CONF_OK;
}


static char *
ngx_http_haskell_write_code(ngx_conf_t *cf, ngx_str_t source_name,
                            ngx_str_t fragment)
{
    ngx_file_t  out;
    ngx_str_t   code;

    code.len = STRLEN(haskell_module_code_head) + fragment.len +
            STRLEN(haskell_module_code_tail);
    code.data = ngx_pnalloc(cf->pool, code.len);
    if (code.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(code.data,
               haskell_module_code_head, STRLEN(haskell_module_code_head));
    ngx_memcpy(code.data + STRLEN(haskell_module_code_head),
               fragment.data, fragment.len);
    ngx_memcpy(code.data + STRLEN(haskell_module_code_head) + fragment.len,
               haskell_module_code_tail, STRLEN(haskell_module_code_tail));

    ngx_memzero(&out, sizeof(ngx_file_t));

    out.name.data = ngx_pnalloc(cf->pool, source_name.len + 1);
    if (out.name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(out.name.data, source_name.data, source_name.len);
    out.name.data[source_name.len] = '\0';
    out.name.len = source_name.len;

    out.fd = ngx_open_file(out.name.data,
                            NGX_FILE_WRONLY, NGX_FILE_TRUNCATE, 0);
    if (out.fd == NGX_INVALID_FILE) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "failed to write haskell source code file");
        return NGX_CONF_ERROR;
    }

    (void) ngx_write_file(&out, code.data, code.len, 0);

    if (ngx_close_file(out.fd) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_ALERT, cf, ngx_errno,
                           "failed to close haskell source code file handle");
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_haskell_compile(ngx_conf_t *cf, void *conf, ngx_str_t source_name)
{
    ngx_http_haskell_main_conf_t  *mcf = conf;

    char                          *compile_cmd;
    ngx_uint_t                     extra_len = 0, passed_len, full_len;

    if (mcf->ghc_extra_flags.len > 0) {
        extra_len = mcf->ghc_extra_flags.len + 1;
    }
    full_len = STRLEN(haskell_compile_cmd) + mcf->lib_path.len +
            source_name.len + extra_len + 2;

    compile_cmd = ngx_pnalloc(cf->pool, full_len);
    if (compile_cmd == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(compile_cmd,
               haskell_compile_cmd, STRLEN(haskell_compile_cmd));
    ngx_memcpy(compile_cmd + STRLEN(haskell_compile_cmd), mcf->lib_path.data,
               mcf->lib_path.len);
    ngx_memcpy(compile_cmd + STRLEN(haskell_compile_cmd) + mcf->lib_path.len,
               " ", 1);
    passed_len = STRLEN(haskell_compile_cmd) + mcf->lib_path.len + 1;
    if (extra_len > 0) {
        ngx_memcpy(compile_cmd + passed_len,
                   mcf->ghc_extra_flags.data, mcf->ghc_extra_flags.len);
        ngx_memcpy(compile_cmd + passed_len + mcf->ghc_extra_flags.len,
                   " ", 1);
        passed_len += extra_len;
    }
    ngx_memcpy(compile_cmd + passed_len, source_name.data, source_name.len);
    compile_cmd[full_len - 1] = '\0';

    if (system(compile_cmd) != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to compile haskell source code file");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_haskell_load(ngx_cycle_t *cycle)
{
    ngx_uint_t                       i;
    ngx_http_haskell_main_conf_t    *mcf;
    ngx_http_haskell_ref_handler_t  *handlers;
    char                            *dl_error;

    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_haskell_module);

    if (mcf->dl_handle != NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "haskell library has been unexpectedly loaded");
        return NGX_ERROR;
    }

    mcf->dl_handle = dlopen((char*) mcf->lib_path.data, RTLD_LAZY);
    if (mcf->dl_handle == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "failed to load compiled haskell library");
        return NGX_ERROR;
    }
    dlerror();

    mcf->hs_init = (void (*)(int *, char ***)) dlsym(mcf->dl_handle, "hs_init");
    dl_error = dlerror();
    if (dl_error != NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "failed to load function \"hs_init\": %s", dl_error);
        return NGX_ERROR;
    }

    mcf->hs_exit = (void (*)(void)) dlsym(mcf->dl_handle, "hs_exit");
    dl_error = dlerror();
    if (dl_error != NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "failed to load function \"hs_exit\": %s", dl_error);
        return NGX_ERROR;
    }

    mcf->hs_add_root = (void (*)(void (*)(void))) dlsym(mcf->dl_handle,
                                                        "hs_add_root");
    dl_error = dlerror();
    if (dl_error != NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "failed to load function \"hs_add_root\": %s", dl_error);
        return NGX_ERROR;
    }

    mcf->init_HsModule = (void (*)(void)) dlsym(mcf->dl_handle,
                                            "__stginit_NgxHaskellUserRuntime" );
    dl_error = dlerror();
    if (dl_error != NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "failed to load function "
                      "\"__stginit_NgxHaskellUserRuntime\": %s", dl_error);
        return NGX_ERROR;
    }

    handlers = mcf->handlers.elts;

    for (i = 0; i < mcf->handlers.nelts; i++) {
        handlers[i].self = dlsym(mcf->dl_handle, (char*) handlers[i].name.data);
        dl_error = dlerror();
        if (dl_error != NULL) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                        "failed to load haskell handler \"%V\": %s",
                        &handlers[i].name, dl_error);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static char *
ngx_http_haskell_run(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_haskell_loc_conf_t       *lcf = conf;

    ngx_uint_t                         i;
    ngx_http_haskell_main_conf_t      *mcf;
    ngx_str_t                         *value;
    ngx_str_t                          handler_name;
    ngx_http_haskell_ref_handler_t    *handlers;
    ngx_http_compile_complex_value_t   ccv1, ccv2;
    ngx_http_variable_t               *v;
    ngx_http_haskell_code_var_data_t  *code_var_data;
    ngx_int_t                          v_idx;
    ngx_uint_t                        *v_idx_ptr;

    const size_t  handler_prefix_size = STRLEN(haskell_module_handler_prefix);

    mcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_haskell_module);

    if (!mcf->code_loaded) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "haskell code was not loaded");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (value[2].len < 2 || value[2].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }
    value[2].len--;
    value[2].data++;

    handler_name.len = value[1].len + handler_prefix_size;
    handler_name.data = ngx_pnalloc(cf->pool, handler_name.len + 1);
    if (handler_name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(handler_name.data,
               haskell_module_handler_prefix, handler_prefix_size);
    ngx_memcpy(handler_name.data + handler_prefix_size,
               value[1].data, value[1].len);
    handler_name.data[handler_name.len] ='\0';

    code_var_data = ngx_array_push(&lcf->code_vars);
    if (code_var_data == NULL) {
        return NGX_CONF_ERROR;
    }

    code_var_data->handler.type = cf->args->nelts == 5 ?
            ngx_http_haskell_handler_type_s_ss :
            ngx_http_haskell_handler_type_s_s;
    code_var_data->handler.name = handler_name;
    code_var_data->handler.index = NGX_ERROR;

    handlers = mcf->handlers.elts;

    for (i = 0; i < mcf->handlers.nelts; i++) {
        if (handler_name.len == handlers[i].name.len
            && ngx_strncmp(handler_name.data, handlers[i].name.data,
                           handler_name.len) == 0)
        {
            code_var_data->handler.index = i;
            break;
        }
    }
    if (code_var_data->handler.index == NGX_ERROR) {
        ngx_http_haskell_ref_handler_t  *handlers_elem;

        handlers_elem = ngx_array_push(&mcf->handlers);
        if (handlers_elem == NULL) {
            return NGX_CONF_ERROR;
        }

        handlers_elem->name = code_var_data->handler.name;
        handlers_elem->self = NULL;
        code_var_data->handler.index = mcf->handlers.nelts - 1;
    }

    v = ngx_http_add_variable(cf, &value[2], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    v_idx = ngx_http_get_variable_index(cf, &value[2]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    v_idx_ptr = ngx_palloc(cf->pool, sizeof(ngx_uint_t));
    if (v_idx_ptr == NULL) {
        return NGX_CONF_ERROR;
    }

    code_var_data->index = v_idx;
    *v_idx_ptr = v_idx;

    v->data = (uintptr_t) v_idx_ptr;
    v->get_handler = ngx_http_haskell_run_handler;

    ngx_memzero(&ccv1, sizeof(ngx_http_compile_complex_value_t));
    ccv1.cf = cf;
    ccv1.value = &value[3];
    ccv1.complex_value = &code_var_data->args[0];

    if (ngx_http_compile_complex_value(&ccv1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts == 5) {
        ngx_memzero(&ccv2, sizeof(ngx_http_compile_complex_value_t));
        ccv2.cf = cf;
        ccv2.value = &value[4];
        ccv2.complex_value = &code_var_data->args[1];

        if (ngx_http_compile_complex_value(&ccv2) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_haskell_run_handler(ngx_http_request_t *r,
                             ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_uint_t                         i;
    ngx_http_haskell_main_conf_t      *mcf;
    ngx_http_haskell_loc_conf_t       *lcf;
    ngx_int_t                         *index = (ngx_int_t *) data;
    ngx_int_t                          found_idx = NGX_ERROR;
    ngx_http_haskell_ref_handler_t    *handlers;
    ngx_array_t                       *code_vars;
    ngx_http_haskell_code_var_data_t  *code_vars_elts;
    ngx_str_t                          arg1, arg2;
    char                              *res;
    u_char                            *res_copy;
    ngx_uint_t                         len;

    if (index == NULL) {
        return NGX_ERROR;
    }

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_haskell_module);

    code_vars = &lcf->code_vars;
    code_vars_elts = code_vars->elts;

    for (i = 0; i < code_vars->nelts; i++) {
        if (*index != code_vars_elts[i].index) {
            continue;
        }
        found_idx = i;
        break;
    }

    if (found_idx == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (ngx_http_complex_value(r, &code_vars_elts[found_idx].args[0], &arg1)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_haskell_module);
    handlers = mcf->handlers.elts;

    if (code_vars_elts[found_idx].handler.type ==
        ngx_http_haskell_handler_type_s_ss)
    {
        if (ngx_http_complex_value(r, &code_vars_elts[found_idx].args[1], &arg2)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        len = ((ngx_http_haskell_handler_s_ss)
               handlers[code_vars_elts[found_idx].handler.index].self)
                    (arg1.data, arg1.len, arg2.data, arg2.len, &res);
    } else {
        len = ((ngx_http_haskell_handler_s_s)
               handlers[code_vars_elts[found_idx].handler.index].self)
                    (arg1.data, arg1.len, &res);
    }
    if (res == NULL) {
        return NGX_ERROR;
    }

    res_copy = ngx_pcalloc(r->pool, len);
    if (res_copy == NULL) {
        ngx_free(res);
        return NGX_ERROR;
    }

    ngx_memcpy(res_copy, res, len);
    ngx_free(res);

    v->len = len;
    v->data = res_copy;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}

#include "php_ffi.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if CFG_STACK_WALKING_TESTS
#include <dlfcn.h> // for dlsym
#endif

const char *datadog_extension_build_id(void) { return ZEND_EXTENSION_BUILD_ID; }
const char *datadog_module_build_id(void) { return ZEND_MODULE_BUILD_ID; }

uint8_t *ddtrace_runtime_id = NULL;

static void locate_ddtrace_runtime_id(const zend_extension *extension) {
    ddtrace_runtime_id = DL_FETCH_SYMBOL(extension->handle, "ddtrace_runtime_id");
}

static void locate_ddtrace_get_profiling_context(const zend_extension *extension) {
    ddtrace_profiling_context (*get_profiling)(void) =
        DL_FETCH_SYMBOL(extension->handle, "ddtrace_get_profiling_context");
    if (EXPECTED(get_profiling)) {
        datadog_php_profiling_get_profiling_context = get_profiling;
    }
}

static bool is_ddtrace_extension(const zend_extension *ext) {
    return ext && ext->name && strcmp(ext->name, "ddtrace") == 0;
}

static ddtrace_profiling_context noop_get_profiling_context(void) {
    return (ddtrace_profiling_context){0, 0};
}

#if CFG_PRELOAD // defined by build.rs
static bool _is_post_startup = false;

bool ddog_php_prof_is_post_startup(void) {
    return _is_post_startup;
}

#if PHP_VERSION_ID < 80000
#define post_startup_cb_result int
#else
#define post_startup_cb_result zend_result
#endif

static post_startup_cb_result (*orig_post_startup_cb)(void) = NULL;

static post_startup_cb_result ddog_php_prof_post_startup_cb(void) {
    if (orig_post_startup_cb) {
        post_startup_cb_result (*cb)(void) = orig_post_startup_cb;

        orig_post_startup_cb = NULL;
        if (cb() != SUCCESS) {
            return FAILURE;
        }
    }

    _is_post_startup = true;

    return SUCCESS;
}
#endif

void datadog_php_profiling_startup(zend_extension *extension) {
    datadog_php_profiling_get_profiling_context = noop_get_profiling_context;

    /* Due to the optional dependency on ddtrace, the profiling module will be
     * loaded after ddtrace if it's present, so ddtrace should always be found
     * on startup and not need a message handler.
     */
    const zend_llist *list = &zend_extensions;
    for (const zend_llist_element *item = list->head; item; item = item->next) {
        const zend_extension *maybe_ddtrace = (zend_extension *)item->data;
        if (maybe_ddtrace != extension && is_ddtrace_extension(maybe_ddtrace)) {
            locate_ddtrace_get_profiling_context(maybe_ddtrace);
            locate_ddtrace_runtime_id(maybe_ddtrace);
            break;
        }
    }

#if CFG_PRELOAD // defined by build.rs
    _is_post_startup = false;
    orig_post_startup_cb = zend_post_startup_cb;
    zend_post_startup_cb = ddog_php_prof_post_startup_cb;
#endif
}

void *datadog_php_profiling_vm_interrupt_addr(void) { return &EG(vm_interrupt); }

zend_module_entry *datadog_get_module_entry(const uint8_t *str, uintptr_t len) {
    return zend_hash_str_find_ptr(&module_registry, (const char *)str, len);
}

ddtrace_profiling_context (*datadog_php_profiling_get_profiling_context)(void) =
    noop_get_profiling_context;

void datadog_php_profiling_install_internal_function_handler(
    datadog_php_profiling_internal_function_handler handler) {
    zend_function *old_handler;
    old_handler = zend_hash_str_find_ptr(CG(function_table), handler.name, handler.name_len);
    if (old_handler != NULL) {
        *handler.old_handler = old_handler->internal_function.handler;
        old_handler->internal_function.handler = handler.new_handler;
    }
}

void datadog_php_profiling_copy_string_view_into_zval(zval *dest, zai_string_view view,
                                                      bool persistent) {
    ZEND_ASSERT(dest);

    if (view.len == 0) {
        if (persistent) {
            ZVAL_EMPTY_PSTRING(dest);
        } else {
            ZVAL_EMPTY_STRING(dest);
        }
    } else {
        ZEND_ASSERT(view.ptr);
        ZVAL_STR(dest, zend_string_init(view.ptr, view.len, persistent));
    }
}

void ddog_php_prof_copy_long_into_zval(zval *dest, long num) {
    ZEND_ASSERT(dest);
    ZVAL_LONG(dest, num);
    return;
}

/**
 * Converts the zend_string pointer into a string view. Null pointers and
 * empty strings will be converted into a string view to a static empty
 * string (single byte of null, len of 0).
 */
zai_string_view ddog_php_prof_zend_string_view(zend_string *zstr) {
    return (!zstr || ZSTR_LEN(zstr) == 0)
        ? ZAI_STRING_EMPTY
        : ZAI_STRING_FROM_ZSTR(zstr);
}

void ddog_php_prof_zend_mm_set_custom_handlers(zend_mm_heap *heap,
                                               void* (*_malloc)(size_t),
                                               void  (*_free)(void*),
                                               void* (*_realloc)(void*, size_t))
{
    zend_mm_set_custom_handlers(heap, _malloc, _free, _realloc);
#if PHP_VERSION_ID < 70300
    if (!_malloc && !_free && !_realloc) {
        memset(heap, ZEND_MM_CUSTOM_HEAP_NONE, sizeof(int));
    }
#endif
}

zend_execute_data* ddog_php_prof_get_current_execute_data()
{
    return EG(current_execute_data);
}

#if CFG_STACK_WALKING_TESTS
static int (*og_snprintf)(char *, size_t, const char *, ...);

// "weak" let's us polyfill, needed by zend_string_init(..., persistent: 1).
void *__attribute__((weak)) __zend_malloc(size_t len) {
    void *tmp = malloc(len);
    if (EXPECTED(tmp || !len)) {
        return tmp;
    }
    fprintf(stderr, "Out of memory\n");
    exit(1);
}

static zend_execute_data *create_fake_frame(int depth) {
    zend_execute_data *execute_data = calloc(1, sizeof(zend_execute_data));
    zend_op_array *op_array = calloc(1, sizeof(zend_function));
    op_array->type = ZEND_USER_FUNCTION;
    execute_data->func = (zend_function *)op_array;

    char buffer[64] = {0};
    int len = og_snprintf(buffer, sizeof buffer, "function name %03d", depth) + 1;
    ZEND_ASSERT(len >= 0 && sizeof buffer > (size_t)len);
    op_array->function_name = zend_string_init(buffer, len - 1, true);

    len = og_snprintf(buffer, sizeof buffer, "filename-%03d.php", depth) + 1;
    ZEND_ASSERT(len >= 0 && sizeof buffer > (size_t)len);
    op_array->filename = zend_string_init(buffer, len - 1, true);

    return execute_data;
}

static zend_execute_data *create_fake_zend_execute_data(int depth) {
    if (depth <= 0) return NULL;
    zend_execute_data *execute_data = create_fake_frame(depth);
    execute_data->prev_execute_data = create_fake_zend_execute_data(depth - 1);
    return execute_data;
}

zend_execute_data *ddog_php_test_create_fake_zend_execute_data(int depth) {
    if (!og_snprintf) {
        og_snprintf = dlsym(RTLD_NEXT, "snprintf");
        if (!og_snprintf) {
            fprintf(stderr, "Failed to locate symbol: %s", dlerror());
            exit(1);
        }
    }

    return create_fake_zend_execute_data(depth);
}

void ddog_php_test_free_fake_zend_execute_data(zend_execute_data *execute_data) {
    if (!execute_data) return;

    ddog_php_test_free_fake_zend_execute_data(execute_data->prev_execute_data);

    if (execute_data->func) {
        // free function name
        if (execute_data->func->common.function_name) {
            free(execute_data->func->common.function_name);
            execute_data->func->common.function_name = NULL;
        }
        // free filename
        if (execute_data->func->op_array.filename) {
            free(execute_data->func->op_array.filename);
            execute_data->func->op_array.filename = NULL;
        }
        // free zend_op_array
        free(execute_data->func);
        execute_data->func = NULL;
    }

    free(execute_data);
}
#endif

void *opcache_handle = NULL;

// OPcache NULLs its handle, so this function will only get the handle during
// MINIT phase. You as the caller has to make sure to only call this function
// during MINIT and not later.
void ddog_php_opcache_init_handle() {
    const zend_llist *list = &zend_extensions;
    zend_extension *maybe_opcache = NULL;
    for (const zend_llist_element *item = list->head; item; item = item->next) {
        maybe_opcache = (zend_extension *)item->data;
        if (maybe_opcache->name && strcmp(maybe_opcache->name, "Zend OPcache") == 0) {
            opcache_handle = maybe_opcache->handle;
            break;
        }
    }
}

// This checks if the JIT actually has a buffer, if so, JIT is active, otherwise
// JIT is inactive. This will only work after OPcache was initialized (after it
// assigned its PHP.INI settings), so make sure to call this in RINIT.a
//
// Attention: this will check for the `opcache.jit_buffer_size` setting as this
// one is a PHP_INI_SYSTEM (can not be changed on a per directory basis). This
// means that the `opcache.jit` setting could be `off` and this function would
// still consider JIT to be enabled, as it could be enabled at anytime (runtime
// and per directory settings).
bool ddog_php_jit_enabled() {
    bool jit = false;

    if (opcache_handle) {
        void (*zend_jit_status)(zval *ret) = DL_FETCH_SYMBOL(opcache_handle, "zend_jit_status");
        if (zend_jit_status == NULL) {
            zend_jit_status = DL_FETCH_SYMBOL(opcache_handle, "_zend_jit_status");
        }
        if (zend_jit_status) {
            zval jit_stats_arr;
            array_init(&jit_stats_arr);
            zend_jit_status(&jit_stats_arr);

            zval *jit_stats = zend_hash_str_find(Z_ARR(jit_stats_arr), ZEND_STRL("jit"));
            zval *jit_buffer = zend_hash_str_find(Z_ARR_P(jit_stats), ZEND_STRL("buffer_size"));
            jit = Z_LVAL_P(jit_buffer) > 0; // JIT is active!

            zval_ptr_dtor(&jit_stats_arr);
        }
    }
    return jit;
}

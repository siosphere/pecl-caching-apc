/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id$ */

#include "apc_php.h"
#include "apc_main.h"
#include "apc.h"
#include "apc_lock.h"
#include "apc_cache.h"
#include "apc_compile.h"
#include "apc_globals.h"
#include "apc_sma.h"
#include "apc_stack.h"
#include "apc_zend.h"
#include "apc_pool.h"
#include "apc_string.h"
#include "SAPI.h"
#include "php_scandir.h"
#include "ext/standard/php_var.h"
#include "ext/standard/md5.h"

#define APC_MAX_SERIALIZERS 16

/* {{{ module variables */

/* pointer to the original Zend engine compile_file function */
typedef zend_op_array* (zend_compile_t)(zend_file_handle*, int TSRMLS_DC);
static zend_compile_t *old_compile_file;
static apc_serializer_t apc_serializers[APC_MAX_SERIALIZERS] = {{0,}};

/* }}} */

/* {{{ get/set old_compile_file (to interact with other extensions that need the compile hook) */
static zend_compile_t* set_compile_hook(zend_compile_t *ptr)
{
    zend_compile_t *retval = old_compile_file;

    if (ptr != NULL) old_compile_file = ptr;
    return retval;
}
/* }}} */

/* {{{ install_function */
static int install_function(apc_function_t fn, apc_context_t* ctxt, int lazy TSRMLS_DC)
{
    int status;
    zend_function *func = apc_copy_function_for_execution(fn.function, ctxt TSRMLS_CC);
    if (func == NULL) {
        return FAILURE;
    }

    status = zend_hash_add(EG(function_table), fn.name, fn.name_len+1, func, sizeof(zend_function), NULL);
    efree(func);

    if (status == FAILURE) {
        /* apc_error("Cannot redeclare %s()" TSRMLS_CC, fn.name); */
    }

    return status;
}
/* }}} */

/* {{{ apc_lookup_function_hook */
int apc_lookup_function_hook(char *name, int len, ulong hash, zend_function **fe) {
    apc_function_t *fn;
    int status = FAILURE;
    apc_context_t ctxt = {0,};
    TSRMLS_FETCH();

    ctxt.pool = apc_pool_create(APC_UNPOOL, apc_php_malloc, apc_php_free, apc_sma_protect, apc_sma_unprotect TSRMLS_CC);
    ctxt.copy = APC_COPY_OUT_OPCODE;

    if(zend_hash_quick_find(APCG(lazy_function_table), name, len, hash, (void**)&fn) == SUCCESS) {
        *fe = apc_copy_function_for_execution(fn->function, &ctxt TSRMLS_CC);
        if (fe == NULL)
            return FAILURE;
        status = zend_hash_add(EG(function_table),
                                  fn->name,
                                  fn->name_len+1,
                                  *fe,
                                  sizeof(zend_function),
                                  NULL);
    }

    return status;
}
/* }}} */

/* {{{ install_class */
static int install_class(apc_class_t cl, apc_context_t* ctxt, int lazy TSRMLS_DC)
{
    zend_class_entry* class_entry = cl.class_entry;
    zend_class_entry* parent = NULL;
    int status;

    /* Special case for mangled names. Mangled names are unique to a file.
     * There is no way two classes with the same mangled name will occur,
     * unless a file is included twice. And if in case, a file is included
     * twice, all mangled name conflicts can be ignored and the class redeclaration
     * error may be deferred till runtime of the corresponding DECLARE_CLASS
     * calls.
     */

    if(cl.name_len != 0 && cl.name[0] == '\0') {
        if(zend_hash_exists(CG(class_table), cl.name, cl.name_len+1)) {
            return SUCCESS;
        }
    }

    if(lazy && cl.name_len != 0 && cl.name[0] != '\0') {
        status = zend_hash_add(APCG(lazy_class_table),
                               cl.name,
                               cl.name_len+1,
                               &cl,
                               sizeof(apc_class_t),
                               NULL);
        if(status == FAILURE) {
            zend_error(E_ERROR, "Cannot redeclare class %s", cl.name);
        }
        return status;
    }

    class_entry =
        apc_copy_class_entry_for_execution(cl.class_entry, ctxt TSRMLS_CC);
    if (class_entry == NULL)
        return FAILURE;


    /* restore parent class pointer for compile-time inheritance */
    if (cl.parent_name != NULL) {
        zend_class_entry** parent_ptr = NULL;
        /*
         * __autoload brings in the old issues with mixed inheritance.
         * When a statically inherited class triggers autoload, it runs
         * afoul of a potential require_once "parent.php" in the previous 
         * line, which when executed provides the parent class, but right
         * now goes and hits __autoload which could fail. 
         * 
         * missing parent == re-compile. 
         *
         * whether __autoload is enabled or not, because __autoload errors
         * cause php to die.
         *
         * Aside: Do NOT pass *strlen(cl.parent_name)+1* because
         * zend_lookup_class_ex does it internally anyway!
         */
        status = zend_lookup_class_ex(cl.parent_name,
                                    strlen(cl.parent_name), 
#ifdef ZEND_ENGINE_2_4
                                    NULL,
#endif
                                    0,
                                    &parent_ptr TSRMLS_CC);
        if (status == FAILURE) {
            if(APCG(report_autofilter)) {
                apc_warning("Dynamic inheritance detected for class %s" TSRMLS_CC, cl.name);
            }
            class_entry->parent = NULL;
            return status;
        }
        else {
            parent = *parent_ptr;
            class_entry->parent = parent;
            zend_do_inheritance(class_entry, parent TSRMLS_CC);
        }
    }

    status = zend_hash_add(EG(class_table),
                           cl.name,
                           cl.name_len+1,
                           &class_entry,
                           sizeof(zend_class_entry*),
                           NULL);

    if (status == FAILURE) {
        apc_error("Cannot redeclare class %s" TSRMLS_CC, cl.name);
    }

    return status;
}
/* }}} */

/* {{{ apc_lookup_class_hook */
int apc_lookup_class_hook(char *name, int len, ulong hash, zend_class_entry ***ce) {

    apc_class_t *cl;
    apc_context_t ctxt = {0,};
    TSRMLS_FETCH();

    if(zend_is_compiling(TSRMLS_C)) { return FAILURE; }

    if(zend_hash_quick_find(APCG(lazy_class_table), name, len, hash, (void**)&cl) == FAILURE) {
        return FAILURE;
    }

    ctxt.pool = apc_pool_create(APC_UNPOOL, apc_php_malloc, apc_php_free, apc_sma_protect, apc_sma_unprotect TSRMLS_CC);
    ctxt.copy = APC_COPY_OUT_OPCODE;

    if(install_class(*cl, &ctxt, 0 TSRMLS_CC) == FAILURE) {
        apc_warning("apc_lookup_class_hook: could not install %s" TSRMLS_CC, name);
        return FAILURE;
    }

    if(zend_hash_quick_find(EG(class_table), name, len, hash, (void**)ce) == FAILURE) {
        apc_warning("apc_lookup_class_hook: known error trying to fetch class %s" TSRMLS_CC, name);
        return FAILURE;
    }

    return SUCCESS;

}
/* }}} */

/* {{{ uninstall_class */
static int uninstall_class(apc_class_t cl TSRMLS_DC)
{
    int status;

    status = zend_hash_del(EG(class_table),
                           cl.name,
                           cl.name_len+1);
    if (status == FAILURE) {
        apc_error("Cannot delete class %s" TSRMLS_CC, cl.name);
    }
    return status;
}
/* }}} */

/* {{{ copy_function_name (taken from zend_builtin_functions.c to ensure future compatibility with APC) */
static int copy_function_name(apc_function_t *pf TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    zval *internal_ar = va_arg(args, zval *),
         *user_ar     = va_arg(args, zval *);
    zend_function *func = pf->function;

    if (hash_key->nKeyLength == 0 || hash_key->arKey[0] == 0) {
        return 0;
    }

    if (func->type == ZEND_INTERNAL_FUNCTION) {
        add_next_index_stringl(internal_ar, hash_key->arKey, hash_key->nKeyLength-1, 1);
    } else if (func->type == ZEND_USER_FUNCTION) {
        add_next_index_stringl(user_ar, hash_key->arKey, hash_key->nKeyLength-1, 1);
    }

    return 0;
}

/* {{{ copy_class_or_interface_name (taken from zend_builtin_functions.c to ensure future compatibility with APC) */
static int copy_class_or_interface_name(apc_class_t *cl TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    zval *array = va_arg(args, zval *);
    zend_uint mask = va_arg(args, zend_uint);
    zend_uint comply = va_arg(args, zend_uint);
    zend_uint comply_mask = (comply)? mask:0;
    zend_class_entry *ce  = cl->class_entry;

    if ((hash_key->nKeyLength==0 || hash_key->arKey[0]!=0)
        && (comply_mask == (ce->ce_flags & mask))) {
        add_next_index_stringl(array, ce->name, ce->name_length, 1);
    }
    return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* }}} */

/* {{{ apc_defined_function_hook */
int apc_defined_function_hook(zval *internal, zval *user) {
    TSRMLS_FETCH();
    zend_hash_apply_with_arguments(APCG(lazy_function_table) 
#ifdef ZEND_ENGINE_2_3
    TSRMLS_CC
#endif
    ,(apply_func_args_t) copy_function_name, 2, internal, user);
  return 1;
}
/* }}} */

/* {{{ apc_declared_class_hook */
int apc_declared_class_hook(zval *classes, zend_uint mask, zend_uint comply) {
    TSRMLS_FETCH();
    zend_hash_apply_with_arguments(APCG(lazy_class_table) 
#ifdef ZEND_ENGINE_2_3
    TSRMLS_CC
#endif
    , (apply_func_args_t) copy_class_or_interface_name, 3, classes, mask, comply);
  return 1;
}
/* }}} */

/* {{{ cached_compile */
static zend_op_array* cached_compile(zend_file_handle* h,
                                        int type,
                                        apc_context_t* ctxt TSRMLS_DC)
{
    apc_cache_entry_t* cache_entry;
    int i, ii;

    cache_entry = (apc_cache_entry_t*) apc_stack_top(APCG(cache_stack));
    assert(cache_entry != NULL);

    if (cache_entry->data.file.classes) {
        int lazy_classes = APCG(lazy_classes);
        for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
            if(install_class(cache_entry->data.file.classes[i], ctxt, lazy_classes TSRMLS_CC) == FAILURE) {
                goto default_compile;
            }
        }
    }

    if (cache_entry->data.file.functions) {
        int lazy_functions = APCG(lazy_functions);
        for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
            install_function(cache_entry->data.file.functions[i], ctxt, lazy_functions TSRMLS_CC);
        }
    }

    apc_do_halt_compiler_register(cache_entry->data.file.filename, cache_entry->data.file.halt_offset TSRMLS_CC);


    return apc_copy_op_array_for_execution(NULL, cache_entry->data.file.op_array, ctxt TSRMLS_CC);

default_compile:

    /* XXX more cleanup in uninstall class, and uninstall_function() should be here too */
    if(cache_entry->data.file.classes) {
        for(ii = 0; ii < i ; ii++) {
            uninstall_class(cache_entry->data.file.classes[ii] TSRMLS_CC);
        }
    }

    apc_stack_pop(APCG(cache_stack)); /* pop out cache_entry */

    apc_cache_release(apc_cache, cache_entry TSRMLS_CC);

    /* cannot free up cache data yet, it maybe in use */

    return NULL;
}
/* }}} */

/* {{{ void apc_compiler_func_table_dtor_hook(void *pDest) */
void apc_compiler_func_table_dtor_hook(void *pDest) {
    zend_function *func = (zend_function *)pDest;
    if (func->type == ZEND_USER_FUNCTION) {
        TSRMLS_FETCH();
        function_add_ref(func);
        zend_hash_next_index_insert(APCG(compiler_hook_func_table), func, sizeof(zend_function), NULL);
    } 
    zend_function_dtor(func);
}
/* }}} */

/* {{{ void apc_compiler_class_table_dtor_hook(void *pDest) */
void apc_compiler_class_table_dtor_hook(void *pDest) {
    zend_class_entry **pce = (zend_class_entry **)pDest;

    if ((*pce)->type == ZEND_USER_CLASS) {
        TSRMLS_FETCH();
        ++(*pce)->refcount;
        zend_hash_next_index_insert(APCG(compiler_hook_class_table), pce, sizeof(zend_class_entry *), NULL);
    }
    destroy_zend_class(pce);
}
/* }}} */

/* {{{ UNLOAD_COMPILER_TABLES_HOOKS() 
 */
#define UNLOAD_COMPILER_TABLES_HOOKS() \
    do { \
        zend_hash_destroy(APCG(compiler_hook_func_table)); \
        FREE_HASHTABLE(APCG(compiler_hook_func_table));  \
        zend_hash_destroy(APCG(compiler_hook_class_table)); \
        FREE_HASHTABLE(APCG(compiler_hook_class_table));  \
        APCG(compiler_hook_func_table) = old_hook_func_table; \
        APCG(compiler_hook_class_table) = old_hook_class_table; \
        if (!(--APCG(compile_nesting))) { \
            CG(class_table)->pDestructor = ZEND_CLASS_DTOR; \
            CG(function_table)->pDestructor = ZEND_FUNCTION_DTOR; \
        } \
    } while (0);
/* }}} */

/* {{{ apc_compile_cache_entry  */
zend_bool apc_compile_cache_entry(apc_cache_key_t *key, zend_file_handle* h, int type, time_t t, zend_op_array** op_array, apc_cache_entry_t** cache_entry TSRMLS_DC) {
    int num_functions, num_classes;
    apc_function_t* alloc_functions;
    zend_op_array* alloc_op_array;
    apc_class_t* alloc_classes;
    char *path;
    apc_context_t ctxt;
    HashTable *old_hook_class_table = NULL, *old_hook_func_table = NULL;

    if (!(APCG(compile_nesting)++)) {
        CG(function_table)->pDestructor = apc_compiler_func_table_dtor_hook;
        CG(class_table)->pDestructor = apc_compiler_class_table_dtor_hook;
    }

    if (APCG(compiler_hook_func_table)) {
        old_hook_func_table = APCG(compiler_hook_func_table);
    }

    if (APCG(compiler_hook_class_table)) {
        old_hook_class_table = APCG(compiler_hook_class_table);
    }

    ALLOC_HASHTABLE(APCG(compiler_hook_func_table));
    zend_hash_init(APCG(compiler_hook_func_table), 8, NULL, ZEND_FUNCTION_DTOR, 0);
    ALLOC_HASHTABLE(APCG(compiler_hook_class_table));
    zend_hash_init(APCG(compiler_hook_class_table), 8, NULL, ZEND_CLASS_DTOR, 0);

    /* remember how many functions and classes existed before compilation */
    num_functions = zend_hash_num_elements(CG(function_table));
    num_classes   = zend_hash_num_elements(CG(class_table));

    zend_try {
        /* compile the file using the default compile function,  *
         * we set *op_array here so we return opcodes during     *
         * a failure.  We should not return prior to this line.  */
        *op_array = old_compile_file(h, type TSRMLS_CC);
        if (*op_array == NULL) {
            UNLOAD_COMPILER_TABLES_HOOKS();
            return FAILURE;
        }
    } zend_catch {
        UNLOAD_COMPILER_TABLES_HOOKS();
        zend_bailout();
    } zend_end_try();

    ctxt.pool = apc_pool_create(APC_MEDIUM_POOL, apc_sma_malloc, apc_sma_free, 
                                                 apc_sma_protect, apc_sma_unprotect TSRMLS_CC);
    if (!ctxt.pool) {
        UNLOAD_COMPILER_TABLES_HOOKS();
        apc_warning("apc_compile_cache_entry: Unable to allocate memory for pool." TSRMLS_CC);
        return FAILURE;
    }
    ctxt.copy = APC_COPY_IN_OPCODE;

    if(APCG(file_md5)) {
        int n;
        unsigned char buf[1024];
        PHP_MD5_CTX context;
        php_stream *stream;
        char *filename;

        if(h->opened_path) {
            filename = h->opened_path;
        } else {
            filename = (char *)h->filename;
        }
        stream = php_stream_open_wrapper(filename, "rb", REPORT_ERRORS | ENFORCE_SAFE_MODE, NULL);
        if(stream) {
            PHP_MD5Init(&context);
            while((n = php_stream_read(stream, (char*)buf, sizeof(buf))) > 0) {
                PHP_MD5Update(&context, buf, n);
            }
            PHP_MD5Final(key->md5, &context);
            php_stream_close(stream);
            if(n<0) {
                apc_warning("Error while reading '%s' for md5 generation." TSRMLS_CC, filename);
            }
        } else {
            apc_warning("Unable to open '%s' for md5 generation." TSRMLS_CC, filename);
        }
    }

    if(!(alloc_op_array = apc_copy_op_array(NULL, *op_array, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    if(!(alloc_functions = apc_copy_new_functions(num_functions, &ctxt TSRMLS_CC))) {
        goto freepool;
    }
    if(!(alloc_classes = apc_copy_new_classes(*op_array, num_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    if (zend_hash_num_elements(APCG(compiler_hook_func_table)) 
            && !(alloc_functions = apc_copy_modified_functions(APCG(compiler_hook_func_table),
                    alloc_functions, num_functions, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    if (zend_hash_num_elements(APCG(compiler_hook_class_table)) 
            && !(alloc_classes = apc_copy_modified_classes(APCG(compiler_hook_class_table),
                    alloc_classes, num_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    path = h->opened_path;
    if (!path && key->type == APC_CACHE_KEY_FPFILE) {
        path = (char*)key->data.fpfile.fullpath;
    }
    if (!path) {
        path = (char *)h->filename;
    }

    apc_debug("2. h->opened_path=[%s]  h->filename=[%s]\n" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

    if(!(*cache_entry = apc_cache_make_file_entry(path, alloc_op_array, alloc_functions, alloc_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }
        
    UNLOAD_COMPILER_TABLES_HOOKS();
    return SUCCESS;

freepool:
    UNLOAD_COMPILER_TABLES_HOOKS();
    apc_pool_destroy(ctxt.pool TSRMLS_CC);
    ctxt.pool = NULL;

    return FAILURE;

}
/* }}} */

/* {{{ apc_get_cache_entry
   Fetches the cache entry for a file */
apc_cache_entry_t* apc_get_cache_entry(zend_file_handle* h TSRMLS_DC)
{
    apc_cache_key_t key;
    time_t t;

    if (!APCG(enabled) || apc_cache_busy(apc_cache)) {
        return NULL;
    }

    t = apc_time();

    if (!apc_cache_make_file_key(&key, h->filename, PG(include_path), t TSRMLS_CC)) {
        return NULL;
    }
    return apc_cache_find(apc_cache, key, t TSRMLS_CC);

}
/* }}} */

/* {{{ my_compile_file
   Overrides zend_compile_file */
static zend_op_array* my_compile_file(zend_file_handle* h,
                                               int type TSRMLS_DC)
{
    apc_cache_key_t key;
    apc_cache_entry_t* cache_entry;
    zend_op_array* op_array = NULL;
    time_t t;
    apc_context_t ctxt = {0,};
    int bailout=0;
    const char* filename = NULL;

    if (!APCG(enabled) || apc_cache_busy(apc_cache)) {
        return old_compile_file(h, type TSRMLS_CC);
    }

    if(h->opened_path) {
        filename = h->opened_path;
    } else {
        filename = h->filename;
    }

    /* check our regular expression filters */
    if (APCG(filters) && APCG(compiled_filters) && filename) {
        int ret = apc_regex_match_array(APCG(compiled_filters), filename);

        if(ret == APC_NEGATIVE_MATCH || (ret != APC_POSITIVE_MATCH && !APCG(cache_by_default))) {
            return old_compile_file(h, type TSRMLS_CC);
        }
    } else if(!APCG(cache_by_default)) {
        return old_compile_file(h, type TSRMLS_CC);
    }
    APCG(current_cache) = apc_cache;


    t = apc_time();

    apc_debug("1. h->opened_path=[%s]  h->filename=[%s]\n" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

    /* try to create a cache key; if we fail, give up on caching */
    if (!apc_cache_make_file_key(&key, h->filename, PG(include_path), t TSRMLS_CC)) {
        return old_compile_file(h, type TSRMLS_CC);
    }

    if(!APCG(force_file_update)) {
        /* search for the file in the cache */
        cache_entry = apc_cache_find(apc_cache, key, t TSRMLS_CC);
        ctxt.force_update = 0;
    } else {
        cache_entry = NULL;
        ctxt.force_update = 1;
    }

    if (cache_entry != NULL) {
        int dummy = 1;
        
        ctxt.pool = apc_pool_create(APC_UNPOOL, apc_php_malloc, apc_php_free,
                                                apc_sma_protect, apc_sma_unprotect TSRMLS_CC);
        if (!ctxt.pool) {
            apc_warning("my_compile_file: Unable to allocate memory for pool." TSRMLS_CC);
            return old_compile_file(h, type TSRMLS_CC);
        }
        ctxt.copy = APC_COPY_OUT_OPCODE;
        
        zend_hash_add(&EG(included_files), cache_entry->data.file.filename, 
                            strlen(cache_entry->data.file.filename)+1,
                            (void *)&dummy, sizeof(int), NULL);

        apc_stack_push(APCG(cache_stack), cache_entry TSRMLS_CC);
        op_array = cached_compile(h, type, &ctxt TSRMLS_CC);

        if(op_array) {
#ifdef APC_FILEHITS
            /* If the file comes from the cache, add it to the global request file list */
            add_next_index_string(APCG(filehits), h->filename, 1);
#endif
            /* this is an unpool, which has no cleanup - this only free's the pool header */
            apc_pool_destroy(ctxt.pool TSRMLS_CC);
            
            /* We might leak fds without this hack */
            if (h->type != ZEND_HANDLE_FILENAME) {
                zend_llist_add_element(&CG(open_files), h); 
            }

            /* save this to free on rshutdown */
            cache_entry->data.file.exec_refcount = op_array->refcount;

            return op_array;
        }
        if(APCG(report_autofilter)) {
            apc_warning("Autofiltering %s" TSRMLS_CC, 
                            (h->opened_path ? h->opened_path : h->filename));
            apc_warning("Recompiling %s" TSRMLS_CC, cache_entry->data.file.filename);
        }
        /* TODO: check what happens with EG(included_files) */
    }

    /* Make sure the mtime reflects the files last known mtime, and we respect max_file_size in the case of fpstat==0 */
    if(key.type == APC_CACHE_KEY_FPFILE) {
        apc_fileinfo_t fileinfo;
        struct stat *tmp_buf = NULL;
        if(!strcmp(SG(request_info).path_translated, h->filename)) {
            tmp_buf = sapi_get_stat(TSRMLS_C);  /* Apache has already done this stat() for us */
        }
        if(tmp_buf) {
            fileinfo.st_buf.sb = *tmp_buf;
        } else {
            if (apc_search_paths(h->filename, PG(include_path), &fileinfo TSRMLS_CC) != 0) {
                apc_debug("Stat failed %s - bailing (%s) (%d)\n" TSRMLS_CC,h->filename,SG(request_info).path_translated);
                return old_compile_file(h, type TSRMLS_CC);
            }
        }
        if (APCG(max_file_size) < fileinfo.st_buf.sb.st_size) { 
            apc_debug("File is too big %s (%ld) - bailing\n" TSRMLS_CC, h->filename, fileinfo.st_buf.sb.st_size);
            return old_compile_file(h, type TSRMLS_CC);
        }
        key.mtime = fileinfo.st_buf.sb.st_mtime;
    }

    HANDLE_BLOCK_INTERRUPTIONS();

#if NONBLOCKING_LOCK_AVAILABLE
    if(APCG(write_lock)) {
        if(!apc_cache_write_lock(apc_cache TSRMLS_CC)) {
            HANDLE_UNBLOCK_INTERRUPTIONS();
            return old_compile_file(h, type TSRMLS_CC);
        }
    }
#endif

    zend_try {
        if (apc_compile_cache_entry(&key, h, type, t, &op_array, &cache_entry TSRMLS_CC) == SUCCESS) {
            ctxt.pool = cache_entry->pool;
            ctxt.copy = APC_COPY_IN_OPCODE;
            if (apc_cache_insert(apc_cache, key, cache_entry, &ctxt, t TSRMLS_CC) != 1) {
                apc_pool_destroy(ctxt.pool TSRMLS_CC);
                ctxt.pool = NULL;
            }
        }
    } zend_catch {
        bailout=1; /* in the event of a bailout, ensure we don't create a dead-lock */
    } zend_end_try();

    APCG(current_cache) = NULL;

#if NONBLOCKING_LOCK_AVAILABLE
    if(APCG(write_lock)) {
        apc_cache_write_unlock(apc_cache TSRMLS_CC);
    }
#endif
    HANDLE_UNBLOCK_INTERRUPTIONS();

    if (bailout) zend_bailout();

    return op_array;
}
/* }}} */

/* {{{ data preload */

extern int _apc_store(char *strkey, int strkey_len, const zval *val, const unsigned int ttl, const int exclusive TSRMLS_DC);

static zval* data_unserialize(const char *filename TSRMLS_DC)
{
    zval* retval;
    long len = 0;
    struct stat sb;
    char *contents, *tmp;
    FILE *fp;
    php_unserialize_data_t var_hash = {0,};

    if(VCWD_STAT(filename, &sb) == -1) {
        return NULL;
    }

    fp = fopen(filename, "rb");

    len = sizeof(char)*sb.st_size;

    tmp = contents = malloc(len);

    if(!contents) {
       return NULL;
    }

    if(fread(contents, 1, len, fp) < 1) {	
      free(contents);
      return NULL;
    }

    MAKE_STD_ZVAL(retval);

    PHP_VAR_UNSERIALIZE_INIT(var_hash);
    
    /* I wish I could use json */
    if(!php_var_unserialize(&retval, (const unsigned char**)&tmp, (const unsigned char*)(contents+len), &var_hash TSRMLS_CC)) {
        zval_ptr_dtor(&retval);
        return NULL;
    }

    PHP_VAR_UNSERIALIZE_DESTROY(var_hash);

    free(contents);
    fclose(fp);

    return retval;
}

static int apc_load_data(const char *data_file TSRMLS_DC)
{
    char *p;
    char key[MAXPATHLEN] = {0,};
    unsigned int key_len;
    zval *data;

    p = strrchr(data_file, DEFAULT_SLASH);

    if(p && p[1]) {
        strlcpy(key, p+1, sizeof(key));
        p = strrchr(key, '.');

        if(p) {
            p[0] = '\0';
            key_len = strlen(key)+1;

            data = data_unserialize(data_file TSRMLS_CC);
            if(data) {
                _apc_store(key, key_len, data, 0, 1 TSRMLS_CC);
            }
            return 1;
        }
    }

    return 0;
}

#ifndef ZTS
static int apc_walk_dir(const char *path TSRMLS_DC)
{
    char file[MAXPATHLEN]={0,};
    int ndir, i;
    char *p = NULL;
    struct dirent **namelist = NULL;

    if ((ndir = php_scandir(path, &namelist, 0, php_alphasort)) > 0)
    {
        for (i = 0; i < ndir; i++)
        {
            /* check for extension */
            if (!(p = strrchr(namelist[i]->d_name, '.'))
                    || (p && strcmp(p, ".data")))
            {
                free(namelist[i]);
                continue;
            }
            snprintf(file, MAXPATHLEN, "%s%c%s",
                    path, DEFAULT_SLASH, namelist[i]->d_name);
            if(!apc_load_data(file TSRMLS_CC))
            {
                /* print error */
            }
            free(namelist[i]);
        }
        free(namelist);
    }

    return 1;
}
#endif

void apc_data_preload(TSRMLS_D)
{
    if(!APCG(preload_path)) return;
#ifndef ZTS
    apc_walk_dir(APCG(preload_path) TSRMLS_CC);
#else 
    apc_error("Cannot load data from apc.preload_path=%s in thread-safe mode" TSRMLS_CC, APCG(preload_path));
#endif

}
/* }}} */

/* {{{ apc_serializer hooks */
static int _apc_register_serializer(const char* name, apc_serialize_t serialize, 
                                    apc_unserialize_t unserialize,
                                    void *config TSRMLS_DC)
{
    int i;
    apc_serializer_t *serializer;

    for(i = 0; i < APC_MAX_SERIALIZERS; i++) {
        serializer = &apc_serializers[i];
        if(!serializer->name) {
            /* empty entry */
            serializer->name = name; /* assumed to be const */
            serializer->serialize = serialize;
            serializer->unserialize = unserialize;
            serializer->config = config;
            if (i < APC_MAX_SERIALIZERS - 1) {
                apc_serializers[i+1].name = NULL;
            }
            return 1;
        }
    }

    return 0;
}

apc_serializer_t* apc_find_serializer(const char* name TSRMLS_DC)
{
    int i;
    apc_serializer_t *serializer;

    for(i = 0; i < APC_MAX_SERIALIZERS; i++) {
        serializer = &apc_serializers[i];
        if(serializer->name && (strcmp(serializer->name, name) == 0)) {
            return serializer;
        }
    }
    return NULL;
}

apc_serializer_t* apc_get_serializers(TSRMLS_D)
{
    return &(apc_serializers[0]);
}
/* }}} */

/* {{{ module init and shutdown */

int apc_module_init(int module_number TSRMLS_DC)
{
    /* apc initialization */
#if APC_MMAP
    apc_sma_init(APCG(shm_segments), APCG(shm_size), APCG(mmap_file_mask) TSRMLS_CC);
#else
    apc_sma_init(APCG(shm_segments), APCG(shm_size), NULL TSRMLS_CC);
#endif
    apc_cache = apc_cache_create(APCG(num_files_hint), APCG(gc_ttl), APCG(ttl) TSRMLS_CC);
    apc_user_cache = apc_cache_create(APCG(user_entries_hint), APCG(gc_ttl), APCG(user_ttl) TSRMLS_CC);

    /* override compilation */
    if (APCG(enable_opcode_cache)) {
        old_compile_file = zend_compile_file;
        zend_compile_file = my_compile_file;
    }

    REGISTER_LONG_CONSTANT("\000apc_magic", (long)&set_compile_hook, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT("\000apc_compile_file", (long)&my_compile_file, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT(APC_SERIALIZER_CONSTANT, (long)&_apc_register_serializer, CONST_PERSISTENT | CONST_CS);

    /* test out the constant function pointer */
    apc_register_serializer("php", APC_SERIALIZER_NAME(php), APC_UNSERIALIZER_NAME(php), NULL TSRMLS_CC);

    assert(apc_serializers[0].name != NULL);

    apc_pool_init();

#ifdef ZEND_ENGINE_2_4
#ifndef ZTS
    apc_interned_strings_init(TSRMLS_C);
#endif
#endif

    apc_data_preload(TSRMLS_C);
    APCG(initialized) = 1;
    return 0;
}

int apc_module_shutdown(TSRMLS_D)
{
    if (!APCG(initialized))
        return 0;

    /* restore compilation */
    /* override compilation */
    if (APCG(enable_opcode_cache)) {
        zend_compile_file = old_compile_file;
    }

    /*
     * In case we got interrupted by a SIGTERM or something else during execution
     * we may have cache entries left on the stack that we need to check to make
     * sure that any functions or classes these may have added to the global function
     * and class tables are removed before we blow away the memory that hold them.
     * 
     * This is merely to remove memory leak warnings - as the process is terminated
     * immediately after shutdown. The following while loop can be removed without
     * affecting anything else.
     */
    while (apc_stack_size(APCG(cache_stack)) > 0) {
        int i;
        apc_cache_entry_t* cache_entry = (apc_cache_entry_t*) apc_stack_pop(APCG(cache_stack));
        if (cache_entry->data.file.functions) {
            for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
                zend_hash_del(EG(function_table),
                    cache_entry->data.file.functions[i].name,
                    cache_entry->data.file.functions[i].name_len+1);
            }
        }
        if (cache_entry->data.file.classes) {
            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);
            }
        }
        apc_cache_release(apc_cache, cache_entry TSRMLS_CC);
    }

#ifdef ZEND_ENGINE_2_4
#ifndef ZTS
    apc_interned_strings_shutdown(TSRMLS_C);
#endif
#endif

    apc_cache_destroy(apc_cache TSRMLS_CC);
    apc_cache_destroy(apc_user_cache TSRMLS_CC);
    apc_sma_cleanup(TSRMLS_C);

    APCG(initialized) = 0;
    return 0;
}

/* }}} */

/* {{{ process init and shutdown */
int apc_process_init(int module_number TSRMLS_DC)
{
    return 0;
}

int apc_process_shutdown(TSRMLS_D)
{
    return 0;
}
/* }}} */

/* {{{ apc_deactivate */
static void apc_deactivate(TSRMLS_D)
{
    /* The execution stack was unwound, which prevented us from decrementing
     * the reference counts on active cache entries in `my_execute`.
     */
    while (apc_stack_size(APCG(cache_stack)) > 0) {
        int i;
        apc_cache_entry_t* cache_entry =
            (apc_cache_entry_t*) apc_stack_pop(APCG(cache_stack));

        if (cache_entry->data.file.classes) {
            zend_class_entry* zce = NULL;
            void ** centry = (void*)(&zce);
            zend_class_entry** pzce = NULL;

            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                centry = (void**)&pzce; /* a triple indirection to get zend_class_entry*** */
                if(zend_hash_find(EG(class_table), 
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1,
                    (void**)centry) == FAILURE)
                {
                    /* double inclusion of conditional classes ends up failing 
                     * this lookup the second time around.
                     */
                    continue;
                }

                zce = *pzce;

                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);

                apc_free_class_entry_after_execution(zce TSRMLS_CC);
                zce = NULL;
            }
        }
#if 0
        if (cache_entry->data.file.functions) {
            zend_function fn, *pfn = NULL;

            for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {

                if (zend_hash_find(EG(function_table),
                            cache_entry->data.file.functions[i].name,
                            cache_entry->data.file.functions[i].name_len+1,
                            (void**)&pfn) == FAILURE) {
                    continue;
                }

                fn = *pfn;
                zend_hash_del(EG(function_table),
                        cache_entry->data.file.functions[i].name,
                        cache_entry->data.file.functions[i].name_len+1);
#if 0
                apc_free_function_after_execution(&fn TSRMLS_CC);
                efree(fn);
#endif
                pfn = NULL;
            }
        }

        /* This is a very special case of apc_free_op_array_after_execution.
           File related op_array->refcount is allocated on unpool for execution
           and would never be freed in zend_execute_scripts() */
#ifdef ZEND_ENGINE_2_4
		if (cache_entry->data.file.exec_refcount) {
			apc_php_free(cache_entry->data.file.exec_refcount TSRMLS_CC);
			cache_entry->data.file.exec_refcount = NULL;
		}
#endif
#endif

        apc_cache_release(apc_cache, cache_entry TSRMLS_CC);
    }
}
/* }}} */

/* {{{ request init and shutdown */

int apc_request_init(TSRMLS_D)
{
    apc_stack_clear(APCG(cache_stack));
    if (!APCG(compiled_filters) && APCG(filters)) {
        /* compile regex filters here to avoid race condition between MINIT of PCRE and APC.
         * This should be moved to apc_cache_create() if this race condition between modules is resolved */
        APCG(compiled_filters) = apc_regex_compile_array(APCG(filters) TSRMLS_CC);
    }

    if (!APCG(serializer) && APCG(serializer_name)) {
        /* Avoid race conditions between MINIT of apc and serializer exts like igbinary */
        APCG(serializer) = apc_find_serializer(APCG(serializer_name) TSRMLS_CC);
    }

#ifdef APC_FILEHITS
    ALLOC_INIT_ZVAL(APCG(filehits));
    array_init(APCG(filehits));
#endif

    return 0;
}

int apc_request_shutdown(TSRMLS_D)
{
    apc_deactivate(TSRMLS_C);

#ifdef APC_FILEHITS
    zval_ptr_dtor(&APCG(filehits));
#endif

    /* As long as regex is compiled per request, it must be freed accordingly.*/
    if (APCG(compiled_filters) && APCG(filters)) {
        apc_regex_destroy_array(APCG(compiled_filters) TSRMLS_CC);
        APCG(compiled_filters) = NULL;
    }

    return 0;
}

/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */

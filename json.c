/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2012 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Omar Kilani <omar@php.net>                                  |
  |          Remi Collet <remi@php.net>                                  |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/html.h"
#include "ext/standard/php_smart_str.h"
#include "php_json.h"
#ifdef HAVE_LIBJSON
#include <json.h>
#else
#include <json-c/json.h>
#endif
#include <zend_exceptions.h>

static PHP_MINFO_FUNCTION(json);

static PHP_FUNCTION(json_encode);
static PHP_FUNCTION(json_decode);
static PHP_FUNCTION(json_last_error);
static PHP_FUNCTION(json_last_error_msg);

static PHP_METHOD(JsonIncrementalParser, __construct);
static PHP_METHOD(JsonIncrementalParser, getError);
static PHP_METHOD(JsonIncrementalParser, parse);
static PHP_METHOD(JsonIncrementalParser, reset);
static PHP_METHOD(JsonIncrementalParser, get);
static PHP_METHOD(JsonIncrementalParser, parseFile);

static const char digits[] = "0123456789abcdef";

zend_class_entry *php_json_serializable_ce, *php_json_parser_ce;

static zend_object_handlers php_json_handlers_parser;

struct php_json_object_parser {
	zend_object     zo;
	json_tokener   *tok;
	json_object    *obj;
	int             options;
};

ZEND_DECLARE_MODULE_GLOBALS(json)

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_json_encode, 0, 0, 1)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, options)
	ZEND_ARG_INFO(0, depth)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_json_decode, 0, 0, 1)
	ZEND_ARG_INFO(0, json)
	ZEND_ARG_INFO(0, assoc)
	ZEND_ARG_INFO(0, depth)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_json_none, 0)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ json_functions[] */
static const zend_function_entry json_functions[] = {
	PHP_FE(json_encode,         arginfo_json_encode)
	PHP_FE(json_decode,         arginfo_json_decode)
	PHP_FE(json_last_error,     arginfo_json_none)
	PHP_FE(json_last_error_msg, arginfo_json_none)
	PHP_FE_END
};
/* }}} */

/* {{{ JsonSerializable methods */
ZEND_BEGIN_ARG_INFO(json_serialize_arginfo, 0)
	/* No arguments */
ZEND_END_ARG_INFO();

static const zend_function_entry json_serializable_interface[] = {
	PHP_ABSTRACT_ME(JsonSerializable, jsonSerialize, json_serialize_arginfo)
	PHP_FE_END
};
/* }}} */

/* {{{ JsonIncrementalParser methods */
ZEND_BEGIN_ARG_INFO_EX(arginfo_parser_create, 0, 0, 0)
	ZEND_ARG_INFO(0, depth)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parser_parse, 0, 0, 1)
	ZEND_ARG_INFO(0, json)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parser_pfile, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parser_get, 0, 0, 0)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()


static const zend_function_entry json_functions_parser[] = {
	PHP_ME(JsonIncrementalParser, __construct, arginfo_parser_create, ZEND_ACC_CTOR|ZEND_ACC_PUBLIC)
	PHP_ME(JsonIncrementalParser, getError,    arginfo_json_none,     ZEND_ACC_PUBLIC)
	PHP_ME(JsonIncrementalParser, reset,       arginfo_json_none,     ZEND_ACC_PUBLIC)
	PHP_ME(JsonIncrementalParser, parse,       arginfo_parser_parse,  ZEND_ACC_PUBLIC)
	PHP_ME(JsonIncrementalParser, parseFile,   arginfo_parser_pfile,  ZEND_ACC_PUBLIC)
	PHP_ME(JsonIncrementalParser, get,         arginfo_parser_get,    ZEND_ACC_PUBLIC)
	PHP_FE_END
};
/* }}} */

/* {{{ php_json_parser_free
	 */
static void php_json_parser_free(void *object TSRMLS_DC)
{
	struct php_json_object_parser *intern = (struct php_json_object_parser *) object;

	if (intern->tok) {
		json_tokener_free(intern->tok);
	}

	zend_object_std_dtor(&intern->zo TSRMLS_CC);
	efree(intern);
}
/* }}} */

/* {{{ php_json_parser_new
 */
static zend_object_value php_json_parser_new(zend_class_entry *class_type TSRMLS_DC)
{
	zend_object_value retval;
	struct php_json_object_parser *intern;

	intern = emalloc(sizeof(struct php_json_object_parser));
	memset(intern, 0, sizeof(struct php_json_object_parser));

	zend_object_std_init(&intern->zo, class_type TSRMLS_CC);
	object_properties_init(&intern->zo, class_type);

	intern->tok = NULL;
	intern->obj = NULL;

	retval.handle = zend_objects_store_put(intern, NULL, php_json_parser_free, NULL TSRMLS_CC);
	retval.handlers = (zend_object_handlers *) &php_json_handlers_parser;

	return retval;
}
/* }}} */

#define REGISTER_PARSER_CLASS_CONST_LONG(const_name, value) \
	zend_declare_class_constant_long(php_json_parser_ce, const_name, sizeof(const_name)-1, value TSRMLS_CC);

/* {{{ MINIT */
static PHP_MINIT_FUNCTION(json)
{
	zend_class_entry ce, ce_parser;

	INIT_CLASS_ENTRY(ce, "JsonSerializable", json_serializable_interface);
	php_json_serializable_ce = zend_register_internal_interface(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce_parser, "JsonIncrementalParser", json_functions_parser);
	ce_parser.create_object = php_json_parser_new;
	php_json_parser_ce = zend_register_internal_class(&ce_parser TSRMLS_CC);
	memcpy(&php_json_handlers_parser, zend_get_std_object_handlers(), sizeof(zend_object_handlers));

	/* Incremental parser return value */
	REGISTER_PARSER_CLASS_CONST_LONG("JSON_PARSER_SUCCESS",  json_tokener_success);
	REGISTER_PARSER_CLASS_CONST_LONG("JSON_PARSER_CONTINUE", json_tokener_continue);

	/* encoder options */
	REGISTER_LONG_CONSTANT("JSON_HEX_TAG",  PHP_JSON_HEX_TAG,  CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_HEX_AMP",  PHP_JSON_HEX_AMP,  CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_HEX_APOS", PHP_JSON_HEX_APOS, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_HEX_QUOT", PHP_JSON_HEX_QUOT, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_FORCE_OBJECT", PHP_JSON_FORCE_OBJECT, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_NUMERIC_CHECK", PHP_JSON_NUMERIC_CHECK, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_UNESCAPED_SLASHES", PHP_JSON_UNESCAPED_SLASHES, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_PRETTY_PRINT", PHP_JSON_PRETTY_PRINT, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_UNESCAPED_UNICODE", PHP_JSON_UNESCAPED_UNICODE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_PARTIAL_OUTPUT_ON_ERROR", PHP_JSON_PARTIAL_OUTPUT_ON_ERROR, CONST_CS | CONST_PERSISTENT);

	/* old parser error codes - kept for compatibility */
	REGISTER_LONG_CONSTANT("JSON_ERROR_STATE_MISMATCH", PHP_JSON_ERROR_STATE_MISMATCH, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_CTRL_CHAR", PHP_JSON_ERROR_CTRL_CHAR, CONST_CS | CONST_PERSISTENT);

	/* encoder error codes */
	REGISTER_LONG_CONSTANT("JSON_ERROR_UTF8", PHP_JSON_ERROR_UTF8, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_RECURSION", PHP_JSON_ERROR_RECURSION, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_INF_OR_NAN", PHP_JSON_ERROR_INF_OR_NAN, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_UNSUPPORTED_TYPE", PHP_JSON_ERROR_UNSUPPORTED_TYPE, CONST_CS | CONST_PERSISTENT);

	/* parser error codes */
	REGISTER_LONG_CONSTANT("JSON_ERROR_NONE", PHP_JSON_ERROR_NONE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_DEPTH", PHP_JSON_ERROR_DEPTH, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_ERROR_SYNTAX", PHP_JSON_ERROR_SYNTAX, CONST_CS | CONST_PERSISTENT);

	/* parser option */
	REGISTER_LONG_CONSTANT("JSON_OBJECT_AS_ARRAY",   PHP_JSON_OBJECT_AS_ARRAY,    CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSON_PARSER_NOTSTRICT",  PHP_JSON_PARSER_NOTSTRICT,   CONST_CS | CONST_PERSISTENT);

	/* Only implemented for 32-63 bits integer */
	REGISTER_LONG_CONSTANT("JSON_BIGINT_AS_STRING",  PHP_JSON_BIGINT_AS_STRING,   CONST_CS | CONST_PERSISTENT);

	/* json-c library information */
#ifdef HAVE_LIBJSON
	REGISTER_LONG_CONSTANT("JSON_C_BUNDLED",         0,                           CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("JSON_C_VERSION",       (char *)json_c_version(),    CONST_CS | CONST_PERSISTENT);
#else
	REGISTER_LONG_CONSTANT("JSON_C_BUNDLED",         1,                           CONST_CS | CONST_PERSISTENT);
	REGISTER_STRING_CONSTANT("JSON_C_VERSION",       JSON_C_VERSION,              CONST_CS | CONST_PERSISTENT);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_GINIT_FUNCTION
*/
static PHP_GINIT_FUNCTION(json)
{
	json_globals->encoder_depth = 0;
	json_globals->error_code = 0;
	json_globals->encode_max_depth = 0;
}
/* }}} */


/* {{{ json_module_entry
 */
zend_module_entry json_module_entry = {
	STANDARD_MODULE_HEADER,
	"json",
	json_functions,
	PHP_MINIT(json),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(json),
	PHP_JSON_VERSION,
	PHP_MODULE_GLOBALS(json),
	PHP_GINIT(json),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_JSON
ZEND_GET_MODULE(json)
#endif

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(json)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "json support", "enabled");
	php_info_print_table_row(2, "json version", PHP_JSON_VERSION);
#ifdef HAVE_LIBJSON
	php_info_print_table_row(2, "JSON-C headers version", JSON_C_VERSION);
	php_info_print_table_row(2, "JSON-C library version", json_c_version());
#else
	php_info_print_table_row(2, "JSON-C version (bundled)", JSON_C_VERSION);
#endif
	php_info_print_table_end();
}
/* }}} */

static void json_escape_string(smart_str *buf, char *s, int len, int options TSRMLS_DC);

static int json_determine_array_type(zval **val TSRMLS_DC) /* {{{ */
{
	int i;
	HashTable *myht = HASH_OF(*val);

	i = myht ? zend_hash_num_elements(myht) : 0;
	if (i > 0) {
		char *key;
		ulong index, idx;
		uint key_len;
		HashPosition pos;

		zend_hash_internal_pointer_reset_ex(myht, &pos);
		idx = 0;
		for (;; zend_hash_move_forward_ex(myht, &pos)) {
			i = zend_hash_get_current_key_ex(myht, &key, &key_len, &index, 0, &pos);
			if (i == HASH_KEY_NON_EXISTANT) {
				break;
			}

			if (i == HASH_KEY_IS_STRING) {
				return PHP_JSON_OUTPUT_OBJECT;
			} else {
				if (index != idx) {
					return PHP_JSON_OUTPUT_OBJECT;
				}
			}
			idx++;
		}
	}

	return PHP_JSON_OUTPUT_ARRAY;
}
/* }}} */

/* {{{ Pretty printing support functions */

static inline void json_pretty_print_char(smart_str *buf, int options, char c TSRMLS_DC) /* {{{ */
{
	if (options & PHP_JSON_PRETTY_PRINT) {
		smart_str_appendc(buf, c);
	}
}
/* }}} */

static inline void json_pretty_print_indent(smart_str *buf, int options TSRMLS_DC) /* {{{ */
{
	int i;

	if (options & PHP_JSON_PRETTY_PRINT) {
		for (i = 0; i < JSON_G(encoder_depth); ++i) {
			smart_str_appendl(buf, "    ", 4);
		}
	}
}
/* }}} */

/* }}} */

static void json_encode_array(smart_str *buf, zval **val, int options TSRMLS_DC) /* {{{ */
{
	int i, r;
	HashTable *myht;

	if (Z_TYPE_PP(val) == IS_ARRAY) {
		myht = HASH_OF(*val);
		r = (options & PHP_JSON_FORCE_OBJECT) ? PHP_JSON_OUTPUT_OBJECT : json_determine_array_type(val TSRMLS_CC);
	} else {
		myht = Z_OBJPROP_PP(val);
		r = PHP_JSON_OUTPUT_OBJECT;
	}

	if (myht && myht->nApplyCount > 1) {
		JSON_G(error_code) = PHP_JSON_ERROR_RECURSION;
		smart_str_appendl(buf, "null", 4);
		return;
	}

	if (r == PHP_JSON_OUTPUT_ARRAY) {
		smart_str_appendc(buf, '[');
	} else {
		smart_str_appendc(buf, '{');
	}

	json_pretty_print_char(buf, options, '\n' TSRMLS_CC);
	++JSON_G(encoder_depth);

	i = myht ? zend_hash_num_elements(myht) : 0;

	if (i > 0)
	{
		char *key;
		zval **data;
		ulong index;
		uint key_len;
		HashPosition pos;
		HashTable *tmp_ht;
		int need_comma = 0;

		zend_hash_internal_pointer_reset_ex(myht, &pos);
		for (;; zend_hash_move_forward_ex(myht, &pos)) {
			i = zend_hash_get_current_key_ex(myht, &key, &key_len, &index, 0, &pos);
			if (i == HASH_KEY_NON_EXISTANT)
				break;

			if (zend_hash_get_current_data_ex(myht, (void **) &data, &pos) == SUCCESS) {
				tmp_ht = HASH_OF(*data);
				if (tmp_ht) {
					tmp_ht->nApplyCount++;
				}

				if (r == PHP_JSON_OUTPUT_ARRAY) {
					if (need_comma) {
						smart_str_appendc(buf, ',');
						json_pretty_print_char(buf, options, '\n' TSRMLS_CC);
					} else {
						need_comma = 1;
					}

					json_pretty_print_indent(buf, options TSRMLS_CC);
					php_json_encode(buf, *data, options TSRMLS_CC);
				} else if (r == PHP_JSON_OUTPUT_OBJECT) {
					if (i == HASH_KEY_IS_STRING) {
						if (key[0] == '\0' && Z_TYPE_PP(val) == IS_OBJECT) {
							/* Skip protected and private members. */
							if (tmp_ht) {
								tmp_ht->nApplyCount--;
							}
							continue;
						}

						if (need_comma) {
							smart_str_appendc(buf, ',');
							json_pretty_print_char(buf, options, '\n' TSRMLS_CC);
						} else {
							need_comma = 1;
						}

						json_pretty_print_indent(buf, options TSRMLS_CC);

						json_escape_string(buf, key, key_len - 1, options & ~PHP_JSON_NUMERIC_CHECK TSRMLS_CC);
						smart_str_appendc(buf, ':');

						json_pretty_print_char(buf, options, ' ' TSRMLS_CC);

						php_json_encode(buf, *data, options TSRMLS_CC);
					} else {
						if (need_comma) {
							smart_str_appendc(buf, ',');
							json_pretty_print_char(buf, options, '\n' TSRMLS_CC);
						} else {
							need_comma = 1;
						}

						json_pretty_print_indent(buf, options TSRMLS_CC);

						smart_str_appendc(buf, '"');
						smart_str_append_long(buf, (long) index);
						smart_str_appendc(buf, '"');
						smart_str_appendc(buf, ':');

						json_pretty_print_char(buf, options, ' ' TSRMLS_CC);

						php_json_encode(buf, *data, options TSRMLS_CC);
					}
				}

				if (tmp_ht) {
					tmp_ht->nApplyCount--;
				}
			}
		}
	}

	if (JSON_G(encoder_depth) > JSON_G(encode_max_depth)) {
		JSON_G(error_code) = PHP_JSON_ERROR_DEPTH;
	}
	--JSON_G(encoder_depth);
	json_pretty_print_char(buf, options, '\n' TSRMLS_CC);
	json_pretty_print_indent(buf, options TSRMLS_CC);

	if (r == PHP_JSON_OUTPUT_ARRAY) {
		smart_str_appendc(buf, ']');
	} else {
		smart_str_appendc(buf, '}');
	}
}
/* }}} */


static int json_utf8_to_utf16(unsigned short *utf16, char utf8[], int len) /* {{{ */
{
	size_t pos = 0, us;
	int j, status;

	if (utf16) {
		/* really convert the utf8 string */
		for (j=0 ; pos < len ; j++) {
			us = php_next_utf8_char((const unsigned char *)utf8, len, &pos, &status);
			if (status != SUCCESS) {
				return -1;
			}
			/* From http://en.wikipedia.org/wiki/UTF16 */
			if (us >= 0x10000) {
				us -= 0x10000;
				utf16[j++] = (unsigned short)((us >> 10) | 0xd800);
				utf16[j] = (unsigned short)((us & 0x3ff) | 0xdc00);
			} else {
				utf16[j] = (unsigned short)us;
			}
		}
	} else {
		/* Only check if utf8 string is valid, and compute utf16 lenght */
		for (j=0 ; pos < len ; j++) {
			us = php_next_utf8_char((const unsigned char *)utf8, len, &pos, &status);
			if (status != SUCCESS) {
				return -1;
			}
			if (us >= 0x10000) {
				j++;
			}
		}
	}
	return j;
}
/* }}} */


static void json_escape_string(smart_str *buf, char *s, int len, int options TSRMLS_DC) /* {{{ */
{
	int pos = 0, ulen = 0;
	unsigned short us;
	unsigned short *utf16;
	size_t newlen;

	if (len == 0) {
		smart_str_appendl(buf, "\"\"", 2);
		return;
	}

	if (options & PHP_JSON_NUMERIC_CHECK) {
		double d;
		int type;
		long p;

		if ((type = is_numeric_string(s, len, &p, &d, 0)) != 0) {
			if (type == IS_LONG) {
				smart_str_append_long(buf, p);
			} else if (type == IS_DOUBLE) {
				if (!zend_isinf(d) && !zend_isnan(d)) {
					char tmp[1024];
					php_gcvt(d, EG(precision), '.', 'e', tmp);
					smart_str_appends(buf, tmp);
				} else {
					JSON_G(error_code) = PHP_JSON_ERROR_INF_OR_NAN;
					smart_str_appendc(buf, '0');
				}
			}
			return;
		}

	}

	utf16 = (options & PHP_JSON_UNESCAPED_UNICODE) ? NULL : (unsigned short *) safe_emalloc(len, sizeof(unsigned short), 0);
	ulen = json_utf8_to_utf16(utf16, s, len);
	if (ulen <= 0) {
		if (utf16) {
			efree(utf16);
		}
		if (ulen < 0) {
			JSON_G(error_code) = PHP_JSON_ERROR_UTF8;
			smart_str_appendl(buf, "null", 4);
		} else {
			smart_str_appendl(buf, "\"\"", 2);
		}
		return;
	}
	if (!(options & PHP_JSON_UNESCAPED_UNICODE)) {
		len = ulen;
	}

	/* pre-allocate for string length plus 2 quotes */
	smart_str_alloc(buf, len+2, 0);
	smart_str_appendc(buf, '"');

	while (pos < len)
	{
		us = (options & PHP_JSON_UNESCAPED_UNICODE) ? s[pos++] : utf16[pos++];

		switch (us)
		{
			case '"':
				if (options & PHP_JSON_HEX_QUOT) {
					smart_str_appendl(buf, "\\u0022", 6);
				} else {
					smart_str_appendl(buf, "\\\"", 2);
				}
				break;

			case '\\':
				smart_str_appendl(buf, "\\\\", 2);
				break;

			case '/':
				if (options & PHP_JSON_UNESCAPED_SLASHES) {
					smart_str_appendc(buf, '/');
				} else {
					smart_str_appendl(buf, "\\/", 2);
				}
				break;

			case '\b':
				smart_str_appendl(buf, "\\b", 2);
				break;

			case '\f':
				smart_str_appendl(buf, "\\f", 2);
				break;

			case '\n':
				smart_str_appendl(buf, "\\n", 2);
				break;

			case '\r':
				smart_str_appendl(buf, "\\r", 2);
				break;

			case '\t':
				smart_str_appendl(buf, "\\t", 2);
				break;

			case '<':
				if (options & PHP_JSON_HEX_TAG) {
					smart_str_appendl(buf, "\\u003C", 6);
				} else {
					smart_str_appendc(buf, '<');
				}
				break;

			case '>':
				if (options & PHP_JSON_HEX_TAG) {
					smart_str_appendl(buf, "\\u003E", 6);
				} else {
					smart_str_appendc(buf, '>');
				}
				break;

			case '&':
				if (options & PHP_JSON_HEX_AMP) {
					smart_str_appendl(buf, "\\u0026", 6);
				} else {
					smart_str_appendc(buf, '&');
				}
				break;

			case '\'':
				if (options & PHP_JSON_HEX_APOS) {
					smart_str_appendl(buf, "\\u0027", 6);
				} else {
					smart_str_appendc(buf, '\'');
				}
				break;

			default:
				if (us >= ' ' && ((options & PHP_JSON_UNESCAPED_UNICODE) || (us & 127) == us)) {
					smart_str_appendc(buf, (unsigned char) us);
				} else {
					smart_str_appendl(buf, "\\u", 2);
					smart_str_appendc(buf, digits[(us & 0xf000) >> 12]);
					smart_str_appendc(buf, digits[(us & 0xf00)  >> 8]);
					smart_str_appendc(buf, digits[(us & 0xf0)   >> 4]);
					smart_str_appendc(buf, digits[(us & 0xf)]);
				}
				break;
		}
	}

	smart_str_appendc(buf, '"');
	if (utf16) {
		efree(utf16);
	}
}
/* }}} */


static void json_encode_serializable_object(smart_str *buf, zval *val, int options TSRMLS_DC) /* {{{ */
{
	zend_class_entry *ce = Z_OBJCE_P(val);
	zval *retval = NULL, fname;
	HashTable* myht;

	if (Z_TYPE_P(val) == IS_ARRAY) {
		myht = HASH_OF(val);
	} else {
		myht = Z_OBJPROP_P(val);
	}

	if (myht && myht->nApplyCount > 1) {
		JSON_G(error_code) = PHP_JSON_ERROR_RECURSION;
		smart_str_appendl(buf, "null", 4);
		return;
	}

	ZVAL_STRING(&fname, "jsonSerialize", 0);

	if (FAILURE == call_user_function_ex(EG(function_table), &val, &fname, &retval, 0, NULL, 1, NULL TSRMLS_CC) || !retval) {
		zend_throw_exception_ex(NULL, 0 TSRMLS_CC, "Failed calling %s::jsonSerialize()", ce->name);
		smart_str_appendl(buf, "null", sizeof("null") - 1);
		return;
	}

	if (EG(exception)) {
		/* Error already raised */
		zval_ptr_dtor(&retval);
		smart_str_appendl(buf, "null", sizeof("null") - 1);
		return;
	}

	if ((Z_TYPE_P(retval) == IS_OBJECT) &&
		(Z_OBJ_HANDLE_P(retval) == Z_OBJ_HANDLE_P(val))) {
		/* Handle the case where jsonSerialize does: return $this; by going straight to encode array */
		json_encode_array(buf, &retval, options TSRMLS_CC);
	} else {
		/* All other types, encode as normal */
		php_json_encode(buf, retval, options TSRMLS_CC);
	}

	zval_ptr_dtor(&retval);
}
/* }}} */

PHP_JSON_API void php_json_encode(smart_str *buf, zval *val, int options TSRMLS_DC) /* {{{ */
{
	switch (Z_TYPE_P(val))
	{
		case IS_NULL:
			smart_str_appendl(buf, "null", 4);
			break;

		case IS_BOOL:
			if (Z_BVAL_P(val)) {
				smart_str_appendl(buf, "true", 4);
			} else {
				smart_str_appendl(buf, "false", 5);
			}
			break;

		case IS_LONG:
			smart_str_append_long(buf, Z_LVAL_P(val));
			break;

		case IS_DOUBLE:
			{
				char tmp[1024];
				double dbl = Z_DVAL_P(val);

				if (!zend_isinf(dbl) && !zend_isnan(dbl)) {
					php_gcvt(dbl, EG(precision), '.', 'e', tmp);
					smart_str_appends(buf, tmp);
				} else {
					JSON_G(error_code) = PHP_JSON_ERROR_INF_OR_NAN;
					smart_str_appendc(buf, '0');
				}
			}
			break;

		case IS_STRING:
			json_escape_string(buf, Z_STRVAL_P(val), Z_STRLEN_P(val), options TSRMLS_CC);
			break;

		case IS_OBJECT:
			if (instanceof_function(Z_OBJCE_P(val), php_json_serializable_ce TSRMLS_CC)) {
				json_encode_serializable_object(buf, val, options TSRMLS_CC);
				break;
			}
			/* fallthrough -- Non-serializable object */
		case IS_ARRAY:
			json_encode_array(buf, &val, options TSRMLS_CC);
			break;

		default:
			JSON_G(error_code) = PHP_JSON_ERROR_UNSUPPORTED_TYPE;
			smart_str_appendl(buf, "null", 4);
			break;
	}

	return;
}
/* }}} */

static void json_object_to_zval(json_object  *new_obj, zval *return_value, int options TSRMLS_DC) /* {{{ */
{
	struct json_object_iterator it, itEnd;
	json_object  *tmpobj;
	json_type     type;
	zval         *tmpval;
	int           i, nb;
	int64_t       i64;
	const char   *key;

	RETVAL_NULL();
	if (new_obj) {
		type = json_object_get_type(new_obj);
		switch (type) {
			case json_type_double:
				RETVAL_DOUBLE(json_object_get_double(new_obj));
				break;

			case json_type_string:
				RETVAL_STRINGL(json_object_get_string(new_obj), json_object_get_string_len(new_obj), 1);
				break;

			case json_type_int:
				i64 = json_object_get_int64(new_obj);
				if (i64==INT64_MAX || i64==INT64_MIN) {
					php_error_docref(NULL TSRMLS_CC, E_NOTICE, "integer overflow detected");
				}
#if SIZEOF_LONG > 4
				RETVAL_LONG(i64);
#else
				if (i64 == (int64_t)json_object_get_int(new_obj)) {
					RETVAL_LONG(json_object_get_int(new_obj));

				} else if (options & PHP_JSON_BIGINT_AS_STRING) {
					RETVAL_STRING(json_object_get_string(new_obj), 1);

				} else {
					RETVAL_DOUBLE(json_object_get_double(new_obj));
				}

#endif
				break;

			case json_type_boolean:
				if (json_object_get_boolean(new_obj)) {
					RETVAL_BOOL(1);
				} else {
					RETVAL_BOOL(0);
				}
				break;

			case json_type_null:
				break;

			case json_type_array:
				array_init(return_value);
				nb = json_object_array_length(new_obj);
				for (i=0 ; i<nb ; i++) {
					MAKE_STD_ZVAL(tmpval);
					json_object_to_zval(json_object_array_get_idx(new_obj, i), tmpval, options TSRMLS_CC);
					add_next_index_zval(return_value, tmpval);
				}
				break;

			case json_type_object:
				if (options & PHP_JSON_OBJECT_AS_ARRAY) {
					array_init(return_value);
				} else { /* OBJECT */
					object_init(return_value);
				}
				it = json_object_iter_begin(new_obj);
				itEnd = json_object_iter_end(new_obj);

				while (!json_object_iter_equal(&it, &itEnd)) {
					MAKE_STD_ZVAL(tmpval);
					key = json_object_iter_peek_name(&it);
					tmpobj  = json_object_iter_peek_value(&it);
					json_object_to_zval(tmpobj, tmpval, options TSRMLS_CC);

					if (options & PHP_JSON_OBJECT_AS_ARRAY) {
						add_assoc_zval(return_value, key, tmpval);
					} else  {
						add_property_zval(return_value, (*key ? key : "_empty_"), tmpval);
						Z_DELREF_P(tmpval);
					}
					json_object_iter_next(&it);
				}
				break;

			default:
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "type '%d' not yet implemented", type);
		}
	}
}

/* }}} */

PHP_JSON_API void php_json_decode_ex(zval *return_value, char *str, int str_len, int options, long depth TSRMLS_DC) /* {{{ */
{
	json_tokener *tok;
	json_object  *new_obj;

	RETVAL_NULL();

	tok = json_tokener_new_ex(depth);
	if (!tok) {
		return;
	}
	if (!(options & PHP_JSON_PARSER_NOTSTRICT)) {
		json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
	}
	new_obj = json_tokener_parse_ex(tok, str, str_len);
	if (json_tokener_get_error(tok)==json_tokener_continue) {
		new_obj = json_tokener_parse_ex(tok, "", -1);
	}

	if (new_obj) {
		json_object_to_zval(new_obj, return_value, options TSRMLS_CC);
		json_object_put(new_obj);
	} else {
		switch (json_tokener_get_error(tok)) {
			case json_tokener_success:
				break;

			case json_tokener_error_depth:
				JSON_G(error_code) = PHP_JSON_ERROR_DEPTH;
				break;

			default:
				JSON_G(error_code) = PHP_JSON_ERROR_SYNTAX;
				JSON_G(parser_code) = json_tokener_get_error(tok);
		}
	}
	json_tokener_free(tok);
}
/* }}} */


/* {{{ proto string json_encode(mixed data [, int options[, int depth]])
   Returns the JSON representation of a value */
static PHP_FUNCTION(json_encode)
{
	zval *parameter;
	smart_str buf = {0};
	long options = 0;
	long depth = JSON_PARSER_DEFAULT_DEPTH;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|ll", &parameter, &options, &depth) == FAILURE) {
		return;
	}

	JSON_G(error_code) = PHP_JSON_ERROR_NONE;

	JSON_G(encode_max_depth) = depth;

	php_json_encode(&buf, parameter, options TSRMLS_CC);

	if (JSON_G(error_code) != PHP_JSON_ERROR_NONE && !(options & PHP_JSON_PARTIAL_OUTPUT_ON_ERROR)) {
		ZVAL_FALSE(return_value);
	} else {
		ZVAL_STRINGL(return_value, buf.c, buf.len, 1);
	}

	smart_str_free(&buf);
}
/* }}} */

/* {{{ proto mixed json_decode(string json [, bool assoc [, long depth [, long options]]])
   Decodes the JSON representation into a PHP value */
static PHP_FUNCTION(json_decode)
{
	char *str;
	int str_len;
	zend_bool assoc = 0; /* return JS objects as PHP objects by default */
	long depth = JSON_PARSER_DEFAULT_DEPTH;
	long options = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|bll", &str, &str_len, &assoc, &depth, &options) == FAILURE) {
		return;
	}

	JSON_G(error_code) = 0;

	if (!str_len) {
		RETURN_NULL();
	}

	/* For BC reasons, the bool $assoc overrides the long $options bit for PHP_JSON_OBJECT_AS_ARRAY */
	if (assoc) {
		options |=  PHP_JSON_OBJECT_AS_ARRAY;
	} else {
		options &= ~PHP_JSON_OBJECT_AS_ARRAY;
	}

	php_json_decode_ex(return_value, str, str_len, options, depth TSRMLS_CC);
}
/* }}} */

/* {{{ proto int json_last_error()
   Returns the error code of the last json_encode() or json_decode() call. */
static PHP_FUNCTION(json_last_error)
{
	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	RETURN_LONG(JSON_G(error_code));
}
/* }}} */

/* {{{ proto string json_last_error_msg()
   Returns the error string of the last json_encode() or json_decode() call. */
static PHP_FUNCTION(json_last_error_msg)
{
	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	switch(JSON_G(error_code)) {
		case PHP_JSON_ERROR_NONE:
			RETURN_STRING("No error", 1);
		case PHP_JSON_ERROR_DEPTH:
			RETURN_STRING("Maximum stack depth exceeded", 1);
		case PHP_JSON_ERROR_STATE_MISMATCH:
			RETURN_STRING("State mismatch (invalid or malformed JSON)", 1);
		case PHP_JSON_ERROR_CTRL_CHAR:
			RETURN_STRING("Control character error, possibly incorrectly encoded", 1);
		case PHP_JSON_ERROR_SYNTAX:
			RETURN_STRING(json_tokener_error_desc(JSON_G(parser_code)), 1);
		case PHP_JSON_ERROR_UTF8:
			RETURN_STRING("Malformed UTF-8 characters, possibly incorrectly encoded", 1);
		case PHP_JSON_ERROR_RECURSION:
			RETURN_STRING("Recursion detected", 1);
		case PHP_JSON_ERROR_INF_OR_NAN:
			RETURN_STRING("Inf and NaN cannot be JSON encoded", 1);
		case PHP_JSON_ERROR_UNSUPPORTED_TYPE:
			RETURN_STRING("Type is not supported", 1);
		default:
			RETURN_STRING("Unknown error", 1);
	}

}
/* }}} */

/* {{{ proto JsonIncrementalParser::__construct([int depth [, int options]])
   Creates new JsonIncrementalParser object
*/
static PHP_METHOD(JsonIncrementalParser, __construct)
{
	long                           depth = JSON_PARSER_DEFAULT_DEPTH, options = 0;
	struct php_json_object_parser *intern;
	zend_error_handling            error_handling;

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);
	zend_replace_error_handling(EH_THROW, NULL, &error_handling TSRMLS_CC);
	if (FAILURE == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ll", &depth, &options)) {
		zend_restore_error_handling(&error_handling TSRMLS_CC);
		return;
	}
	zend_restore_error_handling(&error_handling TSRMLS_CC);

	intern->options = (int)options;
	intern->obj = NULL;
	intern->tok = json_tokener_new_ex(depth);
	if (!intern->tok) {
		zend_throw_exception(zend_exception_get_default(TSRMLS_C), "Can't allocate parser", 0 TSRMLS_CC);
	}
	if (!(options & PHP_JSON_PARSER_NOTSTRICT)) {
		json_tokener_set_flags(intern->tok, JSON_TOKENER_STRICT);
	}

}
/* }}} */

/* {{{ proto long JsonIncrementalParser::getError()
   Get current parser error
*/
static PHP_METHOD(JsonIncrementalParser, getError)
{
	struct php_json_object_parser *intern;

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);

	RETURN_LONG(json_tokener_get_error(intern->tok));
}
/* }}} */

/* {{{ proto long JsonIncrementalParser::reset()
   Reset the parser
*/
static PHP_METHOD(JsonIncrementalParser, reset)
{
	struct php_json_object_parser *intern;

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);
	json_tokener_reset(intern->tok);
	intern->obj = NULL;

	RETURN_LONG(json_tokener_get_error(intern->tok));
}
/* }}} */

/* {{{ proto long JsonIncrementalParser::parse(string json)
   Parse a json encoded string
*/
static PHP_METHOD(JsonIncrementalParser, parse)
{
	struct php_json_object_parser *intern;
	char *str;
	int str_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &str_len) == FAILURE) {
		return;
	}

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);

	intern->obj = json_tokener_parse_ex(intern->tok, str, str_len);

	RETURN_LONG(json_tokener_get_error(intern->tok));
}
/* }}} */

/* {{{ proto mixed JsonIncrementalParser::get([long option])
   Get the parsed result
*/
static PHP_METHOD(JsonIncrementalParser, get)
{
	struct php_json_object_parser *intern;
	long options;

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);

	/* Default options from construct */
	options = intern->options;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &options) == FAILURE) {
		return;
	}

	json_object_to_zval(intern->obj, return_value, options TSRMLS_CC);
}
/* }}} */

/* {{{ proto mixed JsonIncrementalParser::get(string filename)
   Get the parsed result
*/
static PHP_METHOD(JsonIncrementalParser, parseFile)
{
	struct php_json_object_parser *intern;
	char          *file;
	int           file_len, ret = json_tokener_success;
	size_t        len;
	char          buf[1024];
	php_stream    *stream;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "p", &file, &file_len) == FAILURE) {
		return;
	}

	intern = (struct php_json_object_parser *)zend_object_store_get_object(getThis() TSRMLS_CC);

	stream = php_stream_open_wrapper(file, "rb", REPORT_ERRORS, NULL);
	if (!stream) {
		RETURN_FALSE;
	}

	json_tokener_reset(intern->tok);
	intern->obj = NULL;

	do
	{
		php_stream_get_line(stream, buf, sizeof(buf), &len);
		if (len>0) {
			intern->obj = json_tokener_parse_ex(intern->tok, buf, len);
			ret = json_tokener_get_error(intern->tok);
		}
	}
	while (len>0 && ret==json_tokener_continue);

	php_stream_close(stream);

	RETURN_LONG(ret);
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

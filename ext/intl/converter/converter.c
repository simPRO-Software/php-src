/*
   +----------------------------------------------------------------------+
   | PHP Version 7                                                        |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Sara Golemon <pollita@php.net>                              |
   +----------------------------------------------------------------------+
 */

#include "converter.h"
#include "zend_exceptions.h"

#include <unicode/utypes.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>
#include <unicode/ucnv.h>
#include <unicode/ustring.h>

#include "../intl_error.h"
#include "../intl_common.h"

typedef struct _php_converter_object {
	UConverter *src, *dest;
	zend_fcall_info to_cb, from_cb;
	zend_fcall_info_cache to_cache, from_cache;
	intl_error error;
	zend_object obj;
} php_converter_object;


static inline php_converter_object *php_converter_fetch_object(zend_object *obj) {
	return (php_converter_object *)((char*)(obj) - XtOffsetOf(php_converter_object, obj));
}
#define Z_INTL_CONVERTER_P(zv) php_converter_fetch_object(Z_OBJ_P(zv))

static zend_class_entry     *php_converter_ce;
static zend_object_handlers  php_converter_object_handlers;

#define CONV_GET(pzv)  (Z_INTL_CONVERTER_P((pzv)))
#define THROW_UFAILURE(obj, fname, error) php_converter_throw_failure(obj, error, \
                                          fname "() returned error " ZEND_LONG_FMT ": %s", (zend_long)error, u_errorName(error))

/* {{{ php_converter_throw_failure */
static inline void php_converter_throw_failure(php_converter_object *objval, UErrorCode error, const char *format, ...) {
	intl_error *err = objval ? &(objval->error) : NULL;
	char message[1024];
	va_list vargs;

	va_start(vargs, format);
	vsnprintf(message, sizeof(message), format, vargs);
	va_end(vargs);

	intl_errors_set(err, error, message, 1);
}
/* }}} */

/* {{{ php_converter_default_callback */
static void php_converter_default_callback(zval *return_value, zval *zobj, zend_long reason, zval *error) {
	/* Basic functionality so children can call parent::toUCallback() */
	switch (reason) {
		case UCNV_UNASSIGNED:
		case UCNV_ILLEGAL:
		case UCNV_IRREGULAR:
		{
			php_converter_object *objval = (php_converter_object*)CONV_GET(zobj);
			char chars[127];
			int8_t chars_len = sizeof(chars);
			UErrorCode uerror = U_ZERO_ERROR;
            if(!objval->src) {
                php_converter_throw_failure(objval, U_INVALID_STATE_ERROR, "Source Converter has not been initialized yet");
				chars[0] = 0x1A;
				chars[1] = 0;
				chars_len = 1;
				ZEND_TRY_ASSIGN_REF_LONG(error, U_INVALID_STATE_ERROR);
                RETVAL_STRINGL(chars, chars_len);
                return;
            }

			/* Yes, this is fairly wasteful at first glance,
			 * but considering that the alternative is to store
			 * what's sent into setSubstChars() and the fact
			 * that this is an extremely unlikely codepath
			 * I'd rather take the CPU hit here, than waste time
			 * storing a value I'm unlikely to use.
			 */
			ucnv_getSubstChars(objval->src, chars, &chars_len, &uerror);
			if (U_FAILURE(uerror)) {
				THROW_UFAILURE(objval, "ucnv_getSubstChars", uerror);
				chars[0] = 0x1A;
				chars[1] = 0;
				chars_len = 1;
			}
			ZEND_TRY_ASSIGN_REF_LONG(error, uerror);
			RETVAL_STRINGL(chars, chars_len);
		}
	}
}
/* }}} */

/* {{{ proto void UConverter::toUCallback(int $reason,
                                          string $source, string $codeUnits,
                                          int &$error) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_toUCallback_arginfo, 0, ZEND_RETURN_VALUE, 4)
	ZEND_ARG_INFO(0, reason)
	ZEND_ARG_INFO(0, source)
	ZEND_ARG_INFO(0, codeUnits)
	ZEND_ARG_INFO(1, error)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, toUCallback) {
	zend_long reason;
	zval *source, *codeUnits, *error;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "lzzz",
		&reason, &source, &codeUnits, &error) == FAILURE) {
		return;
	}

	php_converter_default_callback(return_value, ZEND_THIS, reason, error);
}
/* }}} */

/* {{{ proto void UConverter::fromUCallback(int $reason,
                                            Array $source, int $codePoint,
                                            int &$error) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_fromUCallback_arginfo, 0, ZEND_RETURN_VALUE, 4)
	ZEND_ARG_INFO(0, reason)
	ZEND_ARG_INFO(0, source)
	ZEND_ARG_INFO(0, codePoint)
	ZEND_ARG_INFO(1, error)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, fromUCallback) {
	zend_long reason;
	zval *source, *codePoint, *error;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "lzzz",
		&reason, &source, &codePoint, &error) == FAILURE) {
		return;
	}

	php_converter_default_callback(return_value, ZEND_THIS, reason, error);
}
/* }}} */

/* {{{ php_converter_check_limits */
static inline zend_bool php_converter_check_limits(php_converter_object *objval, zend_long available, zend_long needed) {
	if (available < needed) {
		php_converter_throw_failure(objval, U_BUFFER_OVERFLOW_ERROR, "Buffer overrun " ZEND_LONG_FMT " bytes needed, " ZEND_LONG_FMT " available", needed, available);
		return 0;
	}
	return 1;
}
/* }}} */

#define TARGET_CHECK(cnvargs, needed) php_converter_check_limits(objval, cnvargs->targetLimit - cnvargs->target, needed)

/* {{{ php_converter_append_toUnicode_target */
static void php_converter_append_toUnicode_target(zval *val, UConverterToUnicodeArgs *args, php_converter_object *objval) {
	switch (Z_TYPE_P(val)) {
		case IS_NULL:
			/* Code unit is being skipped */
			return;
		case IS_LONG:
		{
			zend_long lval = Z_LVAL_P(val);
			if ((lval < 0) || (lval > 0x10FFFF)) {
				php_converter_throw_failure(objval, U_ILLEGAL_ARGUMENT_ERROR, "Invalid codepoint U+%04lx", lval);
				return;
			}
			if (lval > 0xFFFF) {
				/* Supplemental planes U+010000 - U+10FFFF */
				if (TARGET_CHECK(args, 2)) {
					/* TODO: Find the ICU call which does this properly */
					*(args->target++) = (UChar)(((lval - 0x10000) >> 10)   | 0xD800);
					*(args->target++) = (UChar)(((lval - 0x10000) & 0x3FF) | 0xDC00);
				}
				return;
			}
			/* Non-suggogate BMP codepoint */
			if (TARGET_CHECK(args, 1)) {
				*(args->target++) = (UChar)lval;
			}
			return;
		}
		case IS_STRING:
		{
			const char *strval = Z_STRVAL_P(val);
			int i = 0, strlen = Z_STRLEN_P(val);

			while((i != strlen) && TARGET_CHECK(args, 1)) {
				UChar c;
				U8_NEXT(strval, i, strlen, c);
				*(args->target++) = c;
			}
			return;
		}
		case IS_ARRAY:
		{
			HashTable *ht = Z_ARRVAL_P(val);
			zval *tmpzval;

			ZEND_HASH_FOREACH_VAL(ht, tmpzval) {
				php_converter_append_toUnicode_target(tmpzval, args, objval);
			} ZEND_HASH_FOREACH_END();
			return;
		}
		default:
			php_converter_throw_failure(objval, U_ILLEGAL_ARGUMENT_ERROR,
                                                    "toUCallback() specified illegal type for substitution character");
	}
}
/* }}} */

/* {{{ php_converter_to_u_callback */
static void php_converter_to_u_callback(const void *context,
                                        UConverterToUnicodeArgs *args,
                                        const char *codeUnits, int32_t length,
                                        UConverterCallbackReason reason,
                                        UErrorCode *pErrorCode) {
	php_converter_object *objval = (php_converter_object*)context;
	zval retval;
	zval zargs[4];

	ZVAL_LONG(&zargs[0], reason);
	if (args->source) {
		ZVAL_STRINGL(&zargs[1], args->source, args->sourceLimit - args->source);
	} else {
		ZVAL_EMPTY_STRING(&zargs[1]);
	}
	if (codeUnits) {
		ZVAL_STRINGL(&zargs[2], codeUnits, length);
	} else {
		ZVAL_EMPTY_STRING(&zargs[2]);
	}
	ZVAL_LONG(&zargs[3], *pErrorCode);

	objval->to_cb.param_count    = 4;
	objval->to_cb.params = zargs;
	objval->to_cb.retval = &retval;
	objval->to_cb.no_separation  = 0;
	if (zend_call_function(&(objval->to_cb), &(objval->to_cache)) == FAILURE) {
		/* Unlikely */
		php_converter_throw_failure(objval, U_INTERNAL_PROGRAM_ERROR, "Unexpected failure calling toUCallback()");
	} else if (!Z_ISUNDEF(retval)) {
		php_converter_append_toUnicode_target(&retval, args, objval);
		zval_ptr_dtor(&retval);
	}

	if (Z_TYPE(zargs[3]) == IS_LONG) {
		*pErrorCode = Z_LVAL(zargs[3]);
	} else if (Z_ISREF(zargs[3]) && Z_TYPE_P(Z_REFVAL(zargs[3])) == IS_LONG) {
		*pErrorCode = Z_LVAL_P(Z_REFVAL(zargs[3]));
	}

	zval_ptr_dtor(&zargs[0]);
	zval_ptr_dtor(&zargs[1]);
	zval_ptr_dtor(&zargs[2]);
	zval_ptr_dtor(&zargs[3]);
}
/* }}} */

/* {{{ php_converter_append_fromUnicode_target */
static void php_converter_append_fromUnicode_target(zval *val, UConverterFromUnicodeArgs *args, php_converter_object *objval) {
	switch (Z_TYPE_P(val)) {
		case IS_NULL:
			/* Ignore */
			return;
		case IS_LONG:
			if (TARGET_CHECK(args, 1)) {
				*(args->target++) = Z_LVAL_P(val);
			}
			return;
		case IS_STRING:
		{
			size_t vallen = Z_STRLEN_P(val);
			if (TARGET_CHECK(args, vallen)) {
				memcpy(args->target, Z_STRVAL_P(val), vallen);
				args->target += vallen;
			}
			return;
		}
		case IS_ARRAY:
		{
			HashTable *ht = Z_ARRVAL_P(val);
			zval *tmpzval;
			ZEND_HASH_FOREACH_VAL(ht, tmpzval) {
				php_converter_append_fromUnicode_target(tmpzval, args, objval);
			} ZEND_HASH_FOREACH_END();
			return;
		}
		default:
			php_converter_throw_failure(objval, U_ILLEGAL_ARGUMENT_ERROR, "fromUCallback() specified illegal type for substitution character");
	}
}
/* }}} */

/* {{{ php_converter_from_u_callback */
static void php_converter_from_u_callback(const void *context,
                                          UConverterFromUnicodeArgs *args,
                                          const UChar *codeUnits, int32_t length, UChar32 codePoint,
                                          UConverterCallbackReason reason,
                                          UErrorCode *pErrorCode) {
	php_converter_object *objval = (php_converter_object*)context;
	zval retval;
	zval zargs[4];
	int i;

	ZVAL_LONG(&zargs[0], reason);
	array_init(&zargs[1]);
	i = 0;
	while (i < length) {
		UChar32 c;
		U16_NEXT(codeUnits, i, length, c);
		add_next_index_long(&zargs[1], c);
	}
	ZVAL_LONG(&zargs[2], codePoint);
	ZVAL_LONG(&zargs[3], *pErrorCode);

	objval->from_cb.param_count = 4;
	objval->from_cb.params = zargs;
	objval->from_cb.retval = &retval;
	objval->from_cb.no_separation  = 0;
	if (zend_call_function(&(objval->from_cb), &(objval->from_cache)) == FAILURE) {
		/* Unlikely */
		php_converter_throw_failure(objval, U_INTERNAL_PROGRAM_ERROR, "Unexpected failure calling fromUCallback()");
	} else if (!Z_ISUNDEF(retval)) {
		php_converter_append_fromUnicode_target(&retval, args, objval);
		zval_ptr_dtor(&retval);
	}

	if (Z_TYPE(zargs[3]) == IS_LONG) {
		*pErrorCode = Z_LVAL(zargs[3]);
	} else if (Z_ISREF(zargs[3]) && Z_TYPE_P(Z_REFVAL(zargs[3])) == IS_LONG) {
		*pErrorCode = Z_LVAL_P(Z_REFVAL(zargs[3]));
	}

	zval_ptr_dtor(&zargs[0]);
	zval_ptr_dtor(&zargs[1]);
	zval_ptr_dtor(&zargs[2]);
	zval_ptr_dtor(&zargs[3]);
}
/* }}} */

/* {{{ php_converter_set_callbacks */
static inline zend_bool php_converter_set_callbacks(php_converter_object *objval, UConverter *cnv) {
	zend_bool ret = 1;
	UErrorCode error = U_ZERO_ERROR;

	if (objval->obj.ce == php_converter_ce) {
		/* Short-circuit having to go through method calls and data marshalling
		 * when we're using default behavior
		 */
		return 1;
	}

	ucnv_setToUCallBack(cnv, (UConverterToUCallback)php_converter_to_u_callback, (const void*)objval,
                                 NULL, NULL, &error);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(objval, "ucnv_setToUCallBack", error);
		ret = 0;
	}

	error = U_ZERO_ERROR;
	ucnv_setFromUCallBack(cnv, (UConverterFromUCallback)php_converter_from_u_callback, (const void*)objval,
                                    NULL, NULL, &error);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(objval, "ucnv_setFromUCallBack", error);
		ret = 0;
	}
	return ret;
}
/* }}} */

/* {{{ php_converter_set_encoding */
static zend_bool php_converter_set_encoding(php_converter_object *objval,
                                            UConverter **pcnv,
                                            const char *enc, size_t enc_len
                                           ) {
	UErrorCode error = U_ZERO_ERROR;
	UConverter *cnv = ucnv_open(enc, &error);

	if (error == U_AMBIGUOUS_ALIAS_WARNING) {
		UErrorCode getname_error = U_ZERO_ERROR;
		const char *actual_encoding = ucnv_getName(cnv, &getname_error);
		if (U_FAILURE(getname_error)) {
			/* Should never happen */
			actual_encoding = "(unknown)";
		}
		php_error_docref(NULL, E_WARNING, "Ambiguous encoding specified, using %s", actual_encoding);
	} else if (U_FAILURE(error)) {
		if (objval) {
			THROW_UFAILURE(objval, "ucnv_open", error);
		} else {
			php_error_docref(NULL, E_WARNING, "Error setting encoding: %d - %s", (int)error, u_errorName(error));
		}
		return 0;
	}

	if (objval && !php_converter_set_callbacks(objval, cnv)) {
		return 0;
	}

	if (*pcnv) {
		ucnv_close(*pcnv);
	}
	*pcnv = cnv;
	return 1;
}
/* }}} */

/* {{{ php_converter_do_set_encoding */
ZEND_BEGIN_ARG_INFO_EX(php_converter_set_encoding_arginfo, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, encoding)
ZEND_END_ARG_INFO();
static void php_converter_do_set_encoding(UConverter **pcnv, INTERNAL_FUNCTION_PARAMETERS) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	char *enc;
	size_t enc_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &enc, &enc_len) == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR, "Bad arguments, "
				"expected one string argument", 0);
		RETURN_FALSE;
	}
	intl_errors_reset(&objval->error);

	RETURN_BOOL(php_converter_set_encoding(objval, pcnv, enc, enc_len));
}
/* }}} */

/* {{{ proto bool UConverter::setSourceEncoding(string encoding) */
static PHP_METHOD(UConverter, setSourceEncoding) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	php_converter_do_set_encoding(&(objval->src), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ proto bool UConverter::setDestinationEncoding(string encoding) */
static PHP_METHOD(UConverter, setDestinationEncoding) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	php_converter_do_set_encoding(&(objval->dest), INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ php_converter_do_get_encoding */
ZEND_BEGIN_ARG_INFO_EX(php_converter_get_encoding_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static void php_converter_do_get_encoding(php_converter_object *objval, UConverter *cnv, INTERNAL_FUNCTION_PARAMETERS) {
	const char *name;

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR, "Expected no arguments", 0);
		RETURN_FALSE;
	}

	intl_errors_reset(&objval->error);

	if (!cnv) {
		RETURN_NULL();
	}

	name = ucnv_getName(cnv, &objval->error.code);
	if (U_FAILURE(objval->error.code)) {
		THROW_UFAILURE(objval, "ucnv_getName()", objval->error.code);
		RETURN_FALSE;
	}

	RETURN_STRING(name);
}
/* }}} */

/* {{{ proto string UConverter::getSourceEncoding() */
static PHP_METHOD(UConverter, getSourceEncoding) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	php_converter_do_get_encoding(objval, objval->src, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ proto string UConverter::getDestinationEncoding() */
static PHP_METHOD(UConverter, getDestinationEncoding) {
        php_converter_object *objval = CONV_GET(ZEND_THIS);
        php_converter_do_get_encoding(objval, objval->dest, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ php_converter_do_get_type */
ZEND_BEGIN_ARG_INFO_EX(php_converter_get_type_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static void php_converter_do_get_type(php_converter_object *objval, UConverter *cnv, INTERNAL_FUNCTION_PARAMETERS) {
	UConverterType t;

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR, "Expected no arguments", 0);
		RETURN_FALSE;
	}
	intl_errors_reset(&objval->error);

	if (!cnv) {
		RETURN_NULL();
	}

	t = ucnv_getType(cnv);
	if (U_FAILURE(objval->error.code)) {
		THROW_UFAILURE(objval, "ucnv_getType", objval->error.code);
		RETURN_FALSE;
	}

	RETURN_LONG(t);
}
/* }}} */

/* {{{ proto int UConverter::getSourceType() */
static PHP_METHOD(UConverter, getSourceType) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	php_converter_do_get_type(objval, objval->src, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ proto int UConverter::getDestinationType() */
static PHP_METHOD(UConverter, getDestinationType) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	php_converter_do_get_type(objval, objval->dest, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/* {{{ php_converter_resolve_callback */
static void php_converter_resolve_callback(zval *zobj,
                                           php_converter_object *objval,
                                           const char *callback_name,
                                           zend_fcall_info *finfo,
                                           zend_fcall_info_cache *fcache) {
	char *errstr = NULL;
	zval caller;

	array_init(&caller);
	Z_ADDREF_P(zobj);
	add_index_zval(&caller, 0, zobj);
	add_index_string(&caller, 1, callback_name);
	if (zend_fcall_info_init(&caller, 0, finfo, fcache, NULL, &errstr) == FAILURE) {
		php_converter_throw_failure(objval, U_INTERNAL_PROGRAM_ERROR, "Error setting converter callback: %s", errstr);
	}
	zend_array_destroy(Z_ARR(caller));
	ZVAL_UNDEF(&finfo->function_name);
	if (errstr) {
		efree(errstr);
	}
}
/* }}} */

/* {{{ proto UConverter::__construct([string dest = 'utf-8',[string src = 'utf-8']]) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_arginfo, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, destination_encoding)
	ZEND_ARG_INFO(0, source_encoding)
ZEND_END_ARG_INFO();

static PHP_METHOD(UConverter, __construct) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	char *src = "utf-8";
	size_t src_len = sizeof("utf-8") - 1;
	char *dest = src;
	size_t dest_len = src_len;

	intl_error_reset(NULL);

	if (zend_parse_parameters_throw(ZEND_NUM_ARGS(), "|s!s!", &dest, &dest_len, &src, &src_len) == FAILURE) {
		return;
	}

	php_converter_set_encoding(objval, &(objval->src),  src,  src_len );
	php_converter_set_encoding(objval, &(objval->dest), dest, dest_len);
	php_converter_resolve_callback(ZEND_THIS, objval, "toUCallback",   &(objval->to_cb),   &(objval->to_cache));
	php_converter_resolve_callback(ZEND_THIS, objval, "fromUCallback", &(objval->from_cb), &(objval->from_cache));
}
/* }}} */

/* {{{ proto bool UConverter::setSubstChars(string $chars) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_setSubstChars_arginfo, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, chars)
ZEND_END_ARG_INFO();

static PHP_METHOD(UConverter, setSubstChars) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	char *chars;
	size_t chars_len;
	int ret = 1;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &chars, &chars_len) == FAILURE) {
		RETURN_FALSE;
	}
	intl_errors_reset(&objval->error);

	if (objval->src) {
		UErrorCode error = U_ZERO_ERROR;
		ucnv_setSubstChars(objval->src, chars, chars_len, &error);
		if (U_FAILURE(error)) {
			THROW_UFAILURE(objval, "ucnv_setSubstChars", error);
			ret = 0;
		}
	} else {
		php_converter_throw_failure(objval, U_INVALID_STATE_ERROR, "Source Converter has not been initialized yet");
		ret = 0;
	}

	if (objval->dest) {
		UErrorCode error = U_ZERO_ERROR;
		ucnv_setSubstChars(objval->dest, chars, chars_len, &error);
		if (U_FAILURE(error)) {
			THROW_UFAILURE(objval, "ucnv_setSubstChars", error);
			ret = 0;
		}
	} else {
		php_converter_throw_failure(objval, U_INVALID_STATE_ERROR, "Destination Converter has not been initialized yet");
		ret = 0;
	}

	RETURN_BOOL(ret);
}
/* }}} */

/* {{{ proto string UConverter::getSubstChars() */
ZEND_BEGIN_ARG_INFO_EX(php_converter_getSubstChars_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();

static PHP_METHOD(UConverter, getSubstChars) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	char chars[127];
	int8_t chars_len = sizeof(chars);
	UErrorCode error = U_ZERO_ERROR;

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR,
			"UConverter::getSubstChars(): expected no arguments", 0);
		RETURN_FALSE;
	}
	intl_errors_reset(&objval->error);

	if (!objval->src) {
		RETURN_NULL();
	}

	/* src and dest get the same subst chars set,
	 * so it doesn't really matter which one we read from
	 */
	ucnv_getSubstChars(objval->src, chars, &chars_len, &error);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(objval, "ucnv_getSubstChars", error);
		RETURN_FALSE;
	}

	RETURN_STRINGL(chars, chars_len);
}
/* }}} */

/* {{{ php_converter_do_convert */
static zend_string* php_converter_do_convert(UConverter *dest_cnv,
                                             UConverter *src_cnv,  const char *src, int32_t src_len,
                                             php_converter_object *objval
                                            ) {
	UErrorCode	error = U_ZERO_ERROR;
	int32_t		temp_len, ret_len;
	zend_string	*ret;
	UChar		*temp;

	if (!src_cnv || !dest_cnv) {
		php_converter_throw_failure(objval, U_INVALID_STATE_ERROR,
		                            "Internal converters not initialized");
		return NULL;
	}

	/* Get necessary buffer size first */
	temp_len = 1 + ucnv_toUChars(src_cnv, NULL, 0, src, src_len, &error);
	if (U_FAILURE(error) && error != U_BUFFER_OVERFLOW_ERROR) {
		THROW_UFAILURE(objval, "ucnv_toUChars", error);
		return NULL;
	}
	temp = safe_emalloc(sizeof(UChar), temp_len, sizeof(UChar));

	/* Convert to intermediate UChar* array */
	error = U_ZERO_ERROR;
	temp_len = ucnv_toUChars(src_cnv, temp, temp_len, src, src_len, &error);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(objval, "ucnv_toUChars", error);
		efree(temp);
		return NULL;
	}
	temp[temp_len] = 0;

	/* Get necessary output buffer size */
	ret_len = ucnv_fromUChars(dest_cnv, NULL, 0, temp, temp_len, &error);
	if (U_FAILURE(error) && error != U_BUFFER_OVERFLOW_ERROR) {
		THROW_UFAILURE(objval, "ucnv_fromUChars", error);
		efree(temp);
		return NULL;
	}

	ret = zend_string_alloc(ret_len, 0);

	/* Convert to final encoding */
	error = U_ZERO_ERROR;
	ZSTR_LEN(ret) = ucnv_fromUChars(dest_cnv, ZSTR_VAL(ret), ret_len+1, temp, temp_len, &error);
	efree(temp);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(objval, "ucnv_fromUChars", error);
		zend_string_efree(ret);
		return NULL;
	}

	return ret;
}
/* }}} */

/* {{{ proto string UConverter::reasonText(int reason) */
#define UCNV_REASON_CASE(v) case (UCNV_ ## v) : RETURN_STRINGL( "REASON_" #v , sizeof( "REASON_" #v ) - 1);
ZEND_BEGIN_ARG_INFO_EX(php_converter_reasontext_arginfo, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, reason)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, reasonText) {
	zend_long reason;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &reason) == FAILURE) {
		RETURN_FALSE;
	}
	intl_error_reset(NULL);

	switch (reason) {
		UCNV_REASON_CASE(UNASSIGNED)
		UCNV_REASON_CASE(ILLEGAL)
		UCNV_REASON_CASE(IRREGULAR)
		UCNV_REASON_CASE(RESET)
		UCNV_REASON_CASE(CLOSE)
		UCNV_REASON_CASE(CLONE)
		default:
			php_error_docref(NULL, E_WARNING, "Unknown UConverterCallbackReason: " ZEND_LONG_FMT, reason);
			RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto string UConverter::convert(string str[, bool reverse]) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_convert_arginfo, 0, ZEND_RETURN_VALUE, 1)
        ZEND_ARG_INFO(0, str)
	ZEND_ARG_INFO(0, reverse)
ZEND_END_ARG_INFO();

static PHP_METHOD(UConverter, convert) {
        php_converter_object *objval = CONV_GET(ZEND_THIS);
	char *str;
	size_t str_len;
	zend_string *ret;
	zend_bool reverse = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|b",
	                          &str, &str_len, &reverse) == FAILURE) {
		RETURN_FALSE;
	}
	intl_errors_reset(&objval->error);

	ret = php_converter_do_convert(reverse ? objval->src : objval->dest,
	                               reverse ? objval->dest : objval->src,
	                               str,   str_len,
	                               objval);
	if (ret) {
		RETURN_NEW_STR(ret);
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto string UConverter::transcode(string $str, string $toEncoding, string $fromEncoding[, Array $options = array()]) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_transcode_arginfo, 0, ZEND_RETURN_VALUE, 3)
	ZEND_ARG_INFO(0, str)
	ZEND_ARG_INFO(0, toEncoding)
	ZEND_ARG_INFO(0, fromEncoding)
	ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO();

static PHP_METHOD(UConverter, transcode) {
	char *str, *src, *dest;
	size_t str_len, src_len, dest_len;
	zval *options = NULL;
	UConverter *src_cnv = NULL, *dest_cnv = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss|a!",
			&str, &str_len, &dest, &dest_len, &src, &src_len, &options) == FAILURE) {
		RETURN_FALSE;
	}
	intl_error_reset(NULL);

	if (php_converter_set_encoding(NULL, &src_cnv,  src,  src_len) &&
	    php_converter_set_encoding(NULL, &dest_cnv, dest, dest_len)) {
	    zend_string *ret;
		UErrorCode error = U_ZERO_ERROR;

		if (options && zend_hash_num_elements(Z_ARRVAL_P(options))) {
			zval *tmpzval;

			if (U_SUCCESS(error) &&
				(tmpzval = zend_hash_str_find(Z_ARRVAL_P(options), "from_subst", sizeof("from_subst") - 1)) != NULL &&
				Z_TYPE_P(tmpzval) == IS_STRING) {
				error = U_ZERO_ERROR;
				ucnv_setSubstChars(src_cnv, Z_STRVAL_P(tmpzval), Z_STRLEN_P(tmpzval) & 0x7F, &error);
			}
			if (U_SUCCESS(error) &&
				(tmpzval = zend_hash_str_find(Z_ARRVAL_P(options), "to_subst", sizeof("to_subst") - 1)) != NULL &&
				Z_TYPE_P(tmpzval) == IS_STRING) {
				error = U_ZERO_ERROR;
				ucnv_setSubstChars(dest_cnv, Z_STRVAL_P(tmpzval), Z_STRLEN_P(tmpzval) & 0x7F, &error);
			}
		}

		if (U_SUCCESS(error) &&
			(ret = php_converter_do_convert(dest_cnv, src_cnv, str, str_len, NULL)) != NULL) {
			RETVAL_NEW_STR(ret);
		}

		if (U_FAILURE(error)) {
			THROW_UFAILURE(NULL, "transcode", error);
			RETVAL_FALSE;
		}
	} else {
		RETVAL_FALSE;
	}

	if (src_cnv) {
		ucnv_close(src_cnv);
	}
	if (dest_cnv) {
		ucnv_close(dest_cnv);
	}
}
/* }}} */

/* {{{ proto int UConverter::getErrorCode() */
ZEND_BEGIN_ARG_INFO_EX(php_converter_geterrorcode_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, getErrorCode) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR,
			"UConverter::getErrorCode(): expected no arguments", 0);
		RETURN_FALSE;
	}

	RETURN_LONG(intl_error_get_code(&(objval->error)));
}
/* }}} */

/* {{{ proto string UConverter::getErrorMessage() */
ZEND_BEGIN_ARG_INFO_EX(php_converter_geterrormsg_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, getErrorMessage) {
	php_converter_object *objval = CONV_GET(ZEND_THIS);
	zend_string *message = intl_error_get_message(&(objval->error));

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR,
			"UConverter::getErrorMessage(): expected no arguments", 0);
		RETURN_FALSE;
	}

	if (message) {
		RETURN_STR(message);
	} else {
		RETURN_NULL();
	}
}
/* }}} */

/* {{{ proto array UConverter::getAvailable() */
ZEND_BEGIN_ARG_INFO_EX(php_converter_getavailable_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, getAvailable) {
	int32_t i,
			count = ucnv_countAvailable();

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR,
			"UConverter::getErrorMessage(): expected no arguments", 0);
		RETURN_FALSE;
	}
	intl_error_reset(NULL);

	array_init(return_value);
	for(i = 0; i < count; i++) {
		const char *name = ucnv_getAvailableName(i);
		add_next_index_string(return_value, name);
	}
}
/* }}} */

/* {{{ proto array UConverter::getAliases(string name) */
ZEND_BEGIN_ARG_INFO_EX(php_converter_getaliases_arginfo, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, getAliases) {
	char *name;
	size_t name_len;
	UErrorCode error = U_ZERO_ERROR;
	uint16_t i, count;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &name, &name_len) == FAILURE) {
		RETURN_FALSE;
	}
	intl_error_reset(NULL);

	count = ucnv_countAliases(name, &error);
	if (U_FAILURE(error)) {
		THROW_UFAILURE(NULL, "ucnv_countAliases", error);
		RETURN_FALSE;
	}

	array_init(return_value);
	for(i = 0; i < count; i++) {
		const char *alias;

		error = U_ZERO_ERROR;
		alias = ucnv_getAlias(name, i, &error);
		if (U_FAILURE(error)) {
			THROW_UFAILURE(NULL, "ucnv_getAlias", error);
			zend_array_destroy(Z_ARR_P(return_value));
			RETURN_NULL();
		}
		add_next_index_string(return_value, alias);
	}
}
/* }}} */

/* {{{ proto array UConverter::getStandards() */
ZEND_BEGIN_ARG_INFO_EX(php_converter_getstandards_arginfo, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(UConverter, getStandards) {
	uint16_t i, count;

	if (zend_parse_parameters_none() == FAILURE) {
		intl_error_set(NULL, U_ILLEGAL_ARGUMENT_ERROR,
			"UConverter::getStandards(): expected no arguments", 0);
		RETURN_FALSE;
	}
	intl_error_reset(NULL);

	array_init(return_value);
	count = ucnv_countStandards();
	for(i = 0; i < count; i++) {
		UErrorCode error = U_ZERO_ERROR;
		const char *name = ucnv_getStandard(i, &error);
		if (U_FAILURE(error)) {
			THROW_UFAILURE(NULL, "ucnv_getStandard", error);
			zend_array_destroy(Z_ARR_P(return_value));
			RETURN_NULL();
		}
		add_next_index_string(return_value, name);
	}
}
/* }}} */

static const zend_function_entry php_converter_methods[] = {
	PHP_ME(UConverter, __construct,            php_converter_arginfo,                   ZEND_ACC_PUBLIC)

	/* Encoding selection */
	PHP_ME(UConverter, setSourceEncoding,      php_converter_set_encoding_arginfo,      ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, setDestinationEncoding, php_converter_set_encoding_arginfo,      ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, getSourceEncoding,      php_converter_get_encoding_arginfo,      ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, getDestinationEncoding, php_converter_get_encoding_arginfo,      ZEND_ACC_PUBLIC)

	/* Introspection for algorithmic converters */
	PHP_ME(UConverter, getSourceType,          php_converter_get_type_arginfo,          ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, getDestinationType,     php_converter_get_type_arginfo,          ZEND_ACC_PUBLIC)

	/* Basic codeunit error handling */
	PHP_ME(UConverter, getSubstChars,          php_converter_getSubstChars_arginfo,     ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, setSubstChars,          php_converter_setSubstChars_arginfo,     ZEND_ACC_PUBLIC)

	/* Default callback handlers */
	PHP_ME(UConverter, toUCallback,            php_converter_toUCallback_arginfo,       ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, fromUCallback,          php_converter_fromUCallback_arginfo,     ZEND_ACC_PUBLIC)

	/* Core conversion workhorses */
	PHP_ME(UConverter, convert,                php_converter_convert_arginfo,           ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, transcode,              php_converter_transcode_arginfo,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)

	/* Error inspection */
	PHP_ME(UConverter, getErrorCode,           php_converter_geterrorcode_arginfo,      ZEND_ACC_PUBLIC)
	PHP_ME(UConverter, getErrorMessage,        php_converter_geterrormsg_arginfo,       ZEND_ACC_PUBLIC)

	/* Ennumeration and lookup */
	PHP_ME(UConverter, reasonText,             php_converter_reasontext_arginfo,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(UConverter, getAvailable,           php_converter_getavailable_arginfo,      ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(UConverter, getAliases,             php_converter_getaliases_arginfo,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(UConverter, getStandards,           php_converter_getstandards_arginfo,      ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_FE_END
};

/* {{{ Converter create/clone/destroy */
static void php_converter_dtor_object(zend_object *obj) {
	php_converter_object *objval = php_converter_fetch_object(obj);

	if (objval->src) {
		ucnv_close(objval->src);
	}

	if (objval->dest) {
		ucnv_close(objval->dest);
	}

	intl_error_reset(&(objval->error));
}

static zend_object *php_converter_object_ctor(zend_class_entry *ce, php_converter_object **pobjval) {
	php_converter_object *objval;

	objval = zend_object_alloc(sizeof(php_converter_object), ce);

	zend_object_std_init(&objval->obj, ce);
	object_properties_init(&objval->obj, ce);
	intl_error_init(&(objval->error));

	objval->obj.handlers = &php_converter_object_handlers;
	*pobjval = objval;

	return &objval->obj;
}

static zend_object *php_converter_create_object(zend_class_entry *ce) {
	php_converter_object *objval = NULL;
	zend_object *retval = php_converter_object_ctor(ce, &objval);

	object_properties_init(&(objval->obj), ce);

	return retval;
}

static zend_object *php_converter_clone_object(zend_object *object) {
	php_converter_object *objval, *oldobj = php_converter_fetch_object(object);
	zend_object *retval = php_converter_object_ctor(object->ce, &objval);
	UErrorCode error = U_ZERO_ERROR;

	intl_errors_reset(&oldobj->error);

	objval->src = ucnv_safeClone(oldobj->src, NULL, NULL, &error);
	if (U_SUCCESS(error)) {
		error = U_ZERO_ERROR;
		objval->dest = ucnv_safeClone(oldobj->dest, NULL, NULL, &error);
	}
	if (U_FAILURE(error)) {
		zend_string *err_msg;
		THROW_UFAILURE(oldobj, "ucnv_safeClone", error);

		err_msg = intl_error_get_message(&oldobj->error);
		zend_throw_exception(NULL, ZSTR_VAL(err_msg), 0);
		zend_string_release_ex(err_msg, 0);

		return retval;
	}

	/* Update contexts for converter error handlers */
	php_converter_set_callbacks(objval, objval->src );
	php_converter_set_callbacks(objval, objval->dest);

	zend_objects_clone_members(&(objval->obj), &(oldobj->obj));

	/* Newly cloned object deliberately does not inherit error state from original object */

	return retval;
}
/* }}} */

#define CONV_REASON_CONST(v) zend_declare_class_constant_long(php_converter_ce, "REASON_" #v, sizeof("REASON_" #v) - 1, UCNV_ ## v)
#define CONV_TYPE_CONST(v)   zend_declare_class_constant_long(php_converter_ce, #v ,          sizeof(#v) - 1,           UCNV_ ## v)

/* {{{ php_converter_minit */
int php_converter_minit(INIT_FUNC_ARGS) {
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "UConverter", php_converter_methods);
	php_converter_ce = zend_register_internal_class(&ce);
	php_converter_ce->create_object = php_converter_create_object;
	memcpy(&php_converter_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	php_converter_object_handlers.offset = XtOffsetOf(php_converter_object, obj);
	php_converter_object_handlers.clone_obj = php_converter_clone_object;
	php_converter_object_handlers.dtor_obj = php_converter_dtor_object;

	/* enum UConverterCallbackReason */
	CONV_REASON_CONST(UNASSIGNED);
	CONV_REASON_CONST(ILLEGAL);
	CONV_REASON_CONST(IRREGULAR);
	CONV_REASON_CONST(RESET);
	CONV_REASON_CONST(CLOSE);
	CONV_REASON_CONST(CLONE);

	/* enum UConverterType */
	CONV_TYPE_CONST(UNSUPPORTED_CONVERTER);
	CONV_TYPE_CONST(SBCS);
	CONV_TYPE_CONST(DBCS);
	CONV_TYPE_CONST(MBCS);
	CONV_TYPE_CONST(LATIN_1);
	CONV_TYPE_CONST(UTF8);
	CONV_TYPE_CONST(UTF16_BigEndian);
	CONV_TYPE_CONST(UTF16_LittleEndian);
	CONV_TYPE_CONST(UTF32_BigEndian);
	CONV_TYPE_CONST(UTF32_LittleEndian);
	CONV_TYPE_CONST(EBCDIC_STATEFUL);
	CONV_TYPE_CONST(ISO_2022);
	CONV_TYPE_CONST(LMBCS_1);
	CONV_TYPE_CONST(LMBCS_2);
	CONV_TYPE_CONST(LMBCS_3);
	CONV_TYPE_CONST(LMBCS_4);
	CONV_TYPE_CONST(LMBCS_5);
	CONV_TYPE_CONST(LMBCS_6);
	CONV_TYPE_CONST(LMBCS_8);
	CONV_TYPE_CONST(LMBCS_11);
	CONV_TYPE_CONST(LMBCS_16);
	CONV_TYPE_CONST(LMBCS_17);
	CONV_TYPE_CONST(LMBCS_18);
	CONV_TYPE_CONST(LMBCS_19);
	CONV_TYPE_CONST(LMBCS_LAST);
	CONV_TYPE_CONST(HZ);
	CONV_TYPE_CONST(SCSU);
	CONV_TYPE_CONST(ISCII);
	CONV_TYPE_CONST(US_ASCII);
	CONV_TYPE_CONST(UTF7);
	CONV_TYPE_CONST(BOCU1);
	CONV_TYPE_CONST(UTF16);
	CONV_TYPE_CONST(UTF32);
	CONV_TYPE_CONST(CESU8);
	CONV_TYPE_CONST(IMAP_MAILBOX);

	return SUCCESS;
}
/* }}} */

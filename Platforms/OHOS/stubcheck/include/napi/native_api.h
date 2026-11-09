#pragma once

// ============================================================
// napi/native_api.h —— 鸿蒙 napi（stub，仅供 Windows 侧语法自检）
//
// 只声明 NativeEntry.cpp 实际用到的类型与函数；签名与 NDK 头保持一致。
// ============================================================

#include <cstddef>
#include <cstdint>

typedef struct napi_env__*          napi_env;
typedef struct napi_value__*        napi_value;
typedef struct napi_callback_info__* napi_callback_info;

typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);

typedef enum {
    napi_ok = 0,
    napi_invalid_arg = 1,
    napi_generic_failure = 2,
} napi_status;

typedef enum {
    napi_default = 0,
    napi_writable = 1 << 0,
    napi_enumerable = 1 << 1,
    napi_configurable = 1 << 2,
} napi_property_attributes;

typedef struct {
    const char*              utf8name;
    napi_value               name;
    napi_callback            method;
    napi_callback            getter;
    napi_callback            setter;
    napi_value               value;
    napi_property_attributes attributes;
    void*                    data;
} napi_property_descriptor;

typedef struct {
    int        nm_version;
    unsigned   nm_flags;
    const char* nm_filename;
    napi_value (*nm_register_func)(napi_env env, napi_value exports);
    const char* nm_modname;
    void*       nm_priv;
    void*       reserved[4];
} napi_module;

extern "C" napi_status napi_module_register(napi_module* mod);
extern "C" napi_status napi_define_properties(napi_env env, napi_value object, size_t count,
                                              const napi_property_descriptor* properties);
extern "C" napi_status napi_get_cb_info(napi_env env, napi_callback_info info, size_t* argc,
                                        napi_value* argv, napi_value* thisArg, void** data);
extern "C" napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char* buf,
                                                  size_t bufSize, size_t* result);
extern "C" napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result);
extern "C" napi_status napi_get_value_double(napi_env env, napi_value value, double* result);
extern "C" napi_status napi_get_undefined(napi_env env, napi_value* result);

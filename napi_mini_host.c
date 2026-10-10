/*
 * napi_mini_host.c —— 最小 N-API 宿主
 * 用途：在 Wine/Hangover 下直接加载 bdms.node（汽水音乐签名模块），
 *       替代完整 node.exe。bdms.node 通过延迟导入 node.exe 使用 26 个 napi_* 函数，
 *       本宿主实现这些函数 + 手动 patch 延迟导入 IAT。
 *
 * 用法：
 *   napi_host.exe <bdms.node 路径> <url> "<headersLines(\\r\\n 分隔)>" [deviceId]
 *   输出：签名头（name\r\nvalue\r\n...）
 *
 * 编译（x86_64-w64-mingw32-gcc）：
 *   x86_64-w64-mingw32-gcc -O2 -o napi_host.exe napi_mini_host.c -Wl,--export-all-symbols
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winnt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============ 假 JS 值系统 ============ */
typedef enum { NV_UNDEFINED, NV_NULL, NV_BOOL, NV_NUMBER, NV_STRING, NV_OBJECT, NV_FUNCTION } nv_type;

typedef struct nv_value nv_value;
typedef struct nv_prop {
    char *name;
    nv_value *val;
    struct nv_prop *next;
} nv_prop;

/* N-API 类型前置声明 */
typedef void *napi_env;
typedef nv_value *napi_value;
typedef struct napi_callback_info_s *napi_callback_info;
typedef napi_value (*napi_callback)(napi_env env, napi_callback_info info);

struct nv_value {
    nv_type type;
    union {
        int b;                 /* BOOL */
        double num;            /* NUMBER */
        char *str;             /* STRING */
        nv_prop *props;        /* OBJECT */
        struct { napi_callback cb; void *data; char *name; } fn;  /* FUNCTION */
    } u;
};

struct napi_callback_info_s {
    size_t argc;
    napi_value *argv;
    napi_value this_arg;
    void *data;
};

static nv_value *nv_new(nv_type t) {
    nv_value *v = (nv_value *)calloc(1, sizeof(nv_value));
    v->type = t;
    return v;
}
static nv_value *nv_str(const char *s) {
    nv_value *v = nv_new(NV_STRING);
    v->u.str = s ? _strdup(s) : _strdup("");
    return v;
}
static nv_value *nv_obj(void) { return nv_new(NV_OBJECT); }
static nv_value *nv_fn(napi_callback cb, void *data, const char *name) {
    nv_value *v = nv_new(NV_FUNCTION);
    v->u.fn.cb = cb; v->u.fn.data = data;
    v->u.fn.name = name ? _strdup(name) : _strdup("");
    return v;
}
static void nv_set(nv_value *obj, const char *name, nv_value *val) {
    if (!obj || obj->type != NV_OBJECT || !name) return;
    nv_prop *p;
    for (p = obj->u.props; p; p = p->next)
        if (strcmp(p->name, name) == 0) { p->val = val; return; }
    p = (nv_prop *)calloc(1, sizeof(nv_prop));
    p->name = _strdup(name); p->val = val;
    p->next = obj->u.props; obj->u.props = p;
}
static nv_value *nv_get(nv_value *obj, const char *name) {
    if (!obj || obj->type != NV_OBJECT || !name) return NULL;
    nv_prop *p;
    for (p = obj->u.props; p; p = p->next)
        if (strcmp(p->name, name) == 0) return p->val;
    return NULL;
}
static void nv_del(nv_value *v) {
    if (!v) return;
    if (v->type == NV_STRING) free(v->u.str);
    else if (v->type == NV_FUNCTION) { if (v->u.fn.name) free(v->u.fn.name); }
    else if (v->type == NV_OBJECT) {
        nv_prop *p = v->u.props;
        while (p) { nv_prop *n = p->next; free(p->name); nv_del(p->val); free(p); p = n; }
    }
    free(v);
}

/* ============ N-API 状态 ============ */
static nv_value *g_undefined = NULL;
static nv_value *g_last_exception = NULL;
static int g_exception_pending = 0;
static char g_last_error_msg[512] = "";

/* napi_status 枚举（只用到 ok=0） */
typedef enum {
    napi_ok = 0
} napi_status;

/* napi_valuetype 枚举 */
typedef enum {
    napi_undefined = 0, napi_null = 1, napi_boolean = 2, napi_number = 3,
    napi_string = 4, napi_symbol = 5, napi_object = 6, napi_function = 7,
    napi_external = 8, napi_bigint = 9
} napi_valuetype;

typedef struct {
    const char *utf8name;
    napi_value name;
    napi_callback method;
    napi_callback getter;
    napi_callback setter;
    napi_value value;
    napi_valuetype attributes;
    void *data;
} napi_property_descriptor;

typedef struct napi_ref_s { nv_value *val; } napi_ref_s;
typedef napi_ref_s *napi_ref;
typedef void *napi_handle_scope;
typedef void *napi_async_context;

typedef struct {
    const char *error_message;
    void *engine_reserved;
    uint32_t engine_error_code;
    napi_status error_code;
} napi_extended_error_info;

/* ============ N-API 函数实现（全部导出，供 delay import 解析） ============ */

napi_status napi_get_undefined(napi_env env, napi_value *result) {
    if (result) *result = g_undefined ? g_undefined : (g_undefined = nv_new(NV_UNDEFINED));
    return napi_ok;
}

napi_status napi_create_object(napi_env env, napi_value *result) {
    if (result) *result = nv_obj();
    return napi_ok;
}

napi_status napi_create_string_utf8(napi_env env, const char *str, size_t length, napi_value *result) {
    if (result) {
        if (length == (size_t)-1) length = strlen(str);
        char *buf = (char *)malloc(length + 1);
        memcpy(buf, str, length); buf[length] = 0;
        nv_value *v = nv_new(NV_STRING);
        v->u.str = buf;
        *result = v;
    }
    return napi_ok;
}

napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result) {
    if (!value || value->type != NV_STRING) return napi_ok;
    size_t len = strlen(value->u.str);
    if (result) *result = len;
    if (buf && bufsize > 0) {
        size_t cp = len < bufsize - 1 ? len : bufsize - 1;
        memcpy(buf, value->u.str, cp); buf[cp] = 0;
    }
    return napi_ok;
}

napi_status napi_create_function(napi_env env, const char *utf8name, size_t length, napi_callback cb, void *data, napi_value *result) {
    if (result) *result = nv_fn(cb, data, utf8name);
    return napi_ok;
}

napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t *argc, napi_value *argv, napi_value *this_arg, void **data) {
    struct napi_callback_info_s *info = (struct napi_callback_info_s *)cbinfo;
    if (!info) return napi_ok;
    if (argc) *argc = info->argc;
    if (argv && info->argv && info->argc > 0) {
        for (size_t i = 0; i < info->argc; i++) argv[i] = info->argv[i];
    }
    if (this_arg) *this_arg = info->this_arg;
    if (data) *data = info->data;
    return napi_ok;
}

static napi_value nv_key(napi_value key) {
    /* key 可以是字符串值或 undefined；返回字符串值 */
    if (key && key->type == NV_STRING) return key;
    return nv_str("");
}
static napi_value nv_bool(int b) {
    nv_value *v = nv_new(NV_BOOL); v->u.b = b; return v;
}

napi_status napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
    napi_value k = nv_key(key);
    if (object && object->type == NV_OBJECT && k) nv_set(object, k->u.str, value);
    return napi_ok;
}

napi_status napi_get_property(napi_env env, napi_value object, napi_value key, napi_value *result) {
    napi_value k = nv_key(key);
    if (result) *result = NULL;
    if (object && object->type == NV_OBJECT && k) {
        nv_value *v = nv_get(object, k->u.str);
        if (result) *result = v ? v : (g_undefined ? g_undefined : (g_undefined = nv_new(NV_UNDEFINED)));
    }
    return napi_ok;
}

napi_status napi_get_named_property(napi_env env, napi_value object, const char *utf8name, napi_value *result) {
    if (result) *result = NULL;
    if (object && object->type == NV_OBJECT && utf8name) {
        nv_value *v = nv_get(object, utf8name);
        if (result) *result = v ? v : (g_undefined ? g_undefined : (g_undefined = nv_new(NV_UNDEFINED)));
    }
    return napi_ok;
}

napi_status napi_has_property(napi_env env, napi_value object, napi_value key, int *result) {
    napi_value k = nv_key(key);
    if (result) *result = (object && object->type == NV_OBJECT && k && nv_get(object, k->u.str)) ? 1 : 0;
    return napi_ok;
}

napi_status napi_define_properties(napi_env env, napi_value object, size_t property_count,
                                   const napi_property_descriptor *properties) {
    if (!object || object->type != NV_OBJECT) return napi_ok;
    for (size_t i = 0; i < property_count; i++) {
        const napi_property_descriptor *p = &properties[i];
        if (p->utf8name) {
            nv_value *val = NULL;
            if (p->method) val = nv_fn(p->method, p->data, p->utf8name);
            else if (p->value) val = p->value;
            else if (p->getter) val = nv_fn(p->getter, p->data, p->utf8name);
            if (val) nv_set(object, p->utf8name, val);
        } else if (p->name) {
            napi_value k = nv_key(p->name);
            nv_value *val = p->value ? p->value : (p->method ? nv_fn(p->method, p->data, k->u.str) : NULL);
            if (val) nv_set(object, k->u.str, val);
        }
    }
    return napi_ok;
}

napi_status napi_typeof(napi_env env, napi_value value, napi_valuetype *result) {
    if (result) {
        switch (value ? value->type : NV_UNDEFINED) {
            case NV_UNDEFINED: *result = napi_undefined; break;
            case NV_NULL:      *result = napi_null; break;
            case NV_BOOL:      *result = napi_boolean; break;
            case NV_NUMBER:    *result = napi_number; break;
            case NV_STRING:    *result = napi_string; break;
            case NV_OBJECT:    *result = napi_object; break;
            case NV_FUNCTION:  *result = napi_function; break;
            default:           *result = napi_undefined;
        }
    }
    return napi_ok;
}

napi_status napi_open_handle_scope(napi_env env, napi_handle_scope *result) {
    if (result) *result = (void *)1; /* no-op */
    return napi_ok;
}
napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
    return napi_ok; /* no-op */
}

napi_status napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount, napi_ref *result) {
    if (result) {
        napi_ref_s *r = (napi_ref_s *)calloc(1, sizeof(napi_ref_s));
        r->val = value; *result = r;
    }
    return napi_ok;
}
napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value *result) {
    if (result && ref) *result = ref->val;
    else if (result) *result = g_undefined ? g_undefined : (g_undefined = nv_new(NV_UNDEFINED));
    return napi_ok;
}
napi_status napi_delete_reference(napi_env env, napi_ref ref) {
    if (ref) free(ref);
    return napi_ok;
}

napi_status napi_throw(napi_env env, napi_value error) {
    if (error && error->type == NV_STRING)
        snprintf(g_last_error_msg, sizeof(g_last_error_msg), "%s", error->u.str);
    else
        snprintf(g_last_error_msg, sizeof(g_last_error_msg), "napi error thrown");
    g_exception_pending = 1;
    if (g_last_exception) nv_del(g_last_exception);
    g_last_exception = nv_str(g_last_error_msg);
    return napi_ok;
}
napi_status napi_is_exception_pending(napi_env env, int *result) {
    if (result) *result = g_exception_pending;
    return napi_ok;
}
napi_status napi_get_and_clear_last_exception(napi_env env, napi_value *result) {
    if (result) *result = g_last_exception ? g_last_exception : (g_undefined ? g_undefined : (g_undefined = nv_new(NV_UNDEFINED)));
    g_exception_pending = 0;
    return napi_ok;
}
napi_status napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
    if (result) {
        napi_value k = nv_key(msg);
        nv_value *obj = nv_obj();
        nv_set(obj, "message", k);
        if (code) nv_set(obj, "code", code);
        *result = obj;
    }
    return napi_ok;
}
napi_status napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value *result) {
    return napi_create_error(env, code, msg, result);
}
napi_status napi_get_last_error_info(napi_env env, const napi_extended_error_info **result) {
    static napi_extended_error_info info;
    info.error_message = g_last_error_msg;
    info.error_code = napi_ok;
    if (result) *result = &info;
    return napi_ok;
}
void napi_fatal_error(const char *location, size_t location_len, const char *message, size_t message_len) {
    fprintf(stderr, "NAPI FATAL: %.*s\n", (int)message_len, message ? message : "?");
    fflush(stderr);
    exit(1);
}
napi_status napi_add_finalizer(napi_env env, napi_value js_object, void *finalize_data,
                               void (*finalize_cb)(napi_env, void *, void *), void *finalize_hint, napi_value *result) {
    return napi_ok; /* no-op */
}

/* ============ 延迟导入 IAT patch ============ */

static void patch_delay_imports(HMODULE hMod) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)hMod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dd->VirtualAddress || !dd->Size) return;

    HMODULE self = GetModuleHandle(NULL);
    PIMAGE_DELAYLOAD_DESCRIPTOR desc = (PIMAGE_DELAYLOAD_DESCRIPTOR)((BYTE *)hMod + dd->VirtualAddress);
    for (; desc->DllNameRVA != 0; desc++) {
        const char *dll = (const char *)((BYTE *)hMod + desc->DllNameRVA);
        if (lstrcmpiA(dll, "node.exe") != 0) continue;
        uintptr_t *iat = (uintptr_t *)((BYTE *)hMod + desc->ImportAddressTableRVA);
        DWORD oldProtect;
        VirtualProtect((LPVOID)iat, 0x1000, PAGE_READWRITE, &oldProtect);
        uintptr_t *int_ = (uintptr_t *)((BYTE *)hMod + desc->ImportNameTableRVA);
        for (; *int_ != 0; int_++, iat++) {
            if (*int_ & 0x8000000000000000ULL) continue; /* ordinal，跳过 */
            IMAGE_IMPORT_BY_NAME *iname = (IMAGE_IMPORT_BY_NAME *)((BYTE *)hMod + (*int_ & 0x7FFFFFFF));
            const char *fn = (const char *)iname->Name;
            FARPROC p = GetProcAddress(self, fn);
            if (p) *iat = (uintptr_t)p;
            else fprintf(stderr, "[host] 警告: 宿主未导出 %s\n", fn);
        }
    }
}

/* ============ 引擎设置（关键路径） ============ */
static HMODULE g_hBdmsG = NULL;
static void WINAPI hook_setter1(uint64_t v) {
    fprintf(stderr, "[host] SETTER1(0x3970) 被调 rcx=%p -> 写入 0x25e38\n", (void *)v); fflush(stderr);
    *(uint64_t *)((BYTE *)g_hBdmsG + 0x25e38) = v;
}
static void WINAPI hook_setter2(uint64_t v) {
    fprintf(stderr, "[host] SETTER2(0x3980) 被调 rcx=%p -> 写入 0x25e30\n", (void *)v); fflush(stderr);
    *(uint64_t *)((BYTE *)g_hBdmsG + 0x25e30) = v;
}
static void patch_code(HMODULE hBdms, uint32_t rva, void *target) {
    BYTE *p = (BYTE *)hBdms + rva;
    DWORD old;
    VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0x48; p[1] = 0xB8;  /* mov rax, imm64 */
    *(uint64_t *)(p + 2) = (uint64_t)target;
    p[10] = 0xFF; p[11] = 0xE0; /* jmp rax */
    for (int i = 12; i < 16; i++) p[i] = 0x90;
    VirtualProtect(p, 16, old, &old);
    fprintf(stderr, "[host] patched RVA %x -> %p\n", rva, target); fflush(stderr);
}

static void hook_metasecml(HMODULE hBdms) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hBdms;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)hBdms + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)hBdms + dd->VirtualAddress);
    for (; imp->Name; imp++) {
        const char *dll = (const char *)((BYTE *)hBdms + imp->Name);
        if (lstrcmpiA(dll, "metasecml.dll") != 0) continue;
        uintptr_t *iat = (uintptr_t *)((BYTE *)hBdms + imp->FirstThunk);
        uintptr_t *int_ = (uintptr_t *)((BYTE *)hBdms + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        /* IAT 段只读，先放开写权限 */
        DWORD oldProtect;
        VirtualProtect((LPVOID)iat, 0x1000, PAGE_READWRITE, &oldProtect);
        for (; *int_; int_++, iat++) {
            if (*int_ & 0x8000000000000000ULL) continue;
            IMAGE_IMPORT_BY_NAME *iname = (IMAGE_IMPORT_BY_NAME *)((BYTE *)hBdms + (*int_ & 0x7FFFFFFF));
            const char *fn = (const char *)iname->Name;
            (void)fn;
        }
        break;
    }
}

/* ============ 入口 ============ */

typedef napi_value (*napi_register_func)(napi_env env, napi_value exports);

/* ============ base64（常驻 server 模式协议） ============ */
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64_encode(const unsigned char *in, size_t n, size_t *outlen) {
    size_t olen = ((n + 2) / 3) * 4;
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;
    size_t i, o = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        out[o++] = b64tab[(v >> 18) & 63]; out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];  out[o++] = b64tab[v & 63];
    }
    if (i < n) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i+1] << 8;
        out[o++] = b64tab[(v >> 18) & 63]; out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static unsigned char *b64_decode(const char *in, size_t *outlen) {
    size_t n = strlen(in);
    if (n % 4) return NULL;
    size_t olen = (n / 4) * 3;
    unsigned char *out = (unsigned char *)malloc(olen + 1);
    if (!out) return NULL;
    size_t i, o = 0;
    for (i = 0; i < n; i += 4) {
        int a = b64val(in[i]), b = b64val(in[i+1]), c = b64val(in[i+2]), d = b64val(in[i+3]);
        if (a < 0 || b < 0) { free(out); return NULL; }
        unsigned v = (a << 18) | (b << 12);
        if (c >= 0) v |= c << 6;
        if (d >= 0) v |= d;
        out[o++] = (v >> 16) & 0xff;
        if (c >= 0) out[o++] = (v >> 8) & 0xff;
        if (d >= 0) out[o++] = v & 0xff;
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}

int main(int argc, char **argv) {
    g_undefined = nv_new(NV_UNDEFINED);

    if (argc < 3) {
        fprintf(stderr,
            "用法: napi_host.exe <bdms.node路径> <url> \"headers(\\r\\n分隔)\" [deviceId]\n"
            "示例: napi_host.exe Z:\\\\opt\\\\soda\\\\app\\\\resources\\\\app.asar.unpacked\\\\bdms.node \"https://api...\" \"user-agent\\r\\nLunaPC\\r\\n...\" 1\n");
        return 2;
    }
    const char *bdms_path = argv[1];
    const char *url = argv[2];
    const char *headers_lines = argc > 3 ? argv[3] : "";
    const char *device_id = argc > 4 ? argv[4] : "1";

    HMODULE hBdms = LoadLibraryA(bdms_path);
    g_hBdmsG = hBdms;
    if (!hBdms) {
        fprintf(stderr, "[host] LoadLibrary(%s) 失败: %lu\n", bdms_path, GetLastError());
        return 1;
    }
    fprintf(stderr, "[host] bdms.node 已加载，patch metasecml hook + 延迟导入表...\n");
    hook_metasecml(hBdms);
    patch_code(hBdms, 0x3970, (void *)&hook_setter1);
    patch_code(hBdms, 0x3980, (void *)&hook_setter2);
    patch_delay_imports(hBdms);

    napi_register_func reg = (napi_register_func)GetProcAddress(hBdms, "napi_register_module_v1");
    if (!reg) { fprintf(stderr, "[host] 找不到 napi_register_module_v1\n"); return 1; }

    napi_env env = NULL;
    nv_value *exports = nv_obj();
    napi_value mod = reg(env, exports);
    if (g_exception_pending) {
        fprintf(stderr, "[host] 注册期间异常: %s\n", g_last_error_msg);
    }
    nv_value *mod_obj = mod ? mod : (nv_value *)exports;

    nv_value *fn_init = nv_get(mod_obj, "init");
    nv_value *fn_gen = nv_get(mod_obj, "generateHttpSignatureHeaders");
    if (!fn_init || fn_init->type != NV_FUNCTION || !fn_gen || fn_gen->type != NV_FUNCTION) {
        fprintf(stderr, "[host] 导出函数缺失: init=%p gen=%p\n", (void *)fn_init, (void *)fn_gen);
        return 1;
    }
    {
        uint64_t *g0 = (uint64_t *)((BYTE *)hBdms + 0x25e40);
        uint64_t *g1 = (uint64_t *)((BYTE *)hBdms + 0x25e30);
        uint64_t *g2 = (uint64_t *)((BYTE *)hBdms + 0x25e38);
        fprintf(stderr, "[host] g_obj(0x25e40)=%p g1=%p g2=%p\n", (void *)*g0, (void *)*g1, (void *)*g2);
        if (*g0 > 0x10000) {
            uint64_t *obj = (uint64_t *)*g0;
            fprintf(stderr, "[host]   obj[0]=%p obj[2](+0x10)=%p obj[0x88](+0x440)=%p obj[0x8c](+0x460)=%p obj[0x8d](+0x468)=%llx\n",
                (void *)obj[0], (void *)obj[2], (void *)obj[0x440/8], (void *)obj[0x460/8], obj[0x468/8]);
            fprintf(stderr, "[host]   obj[1](+8)=%p obj[3](+0x18)=%p\n", (void *)obj[1], (void *)obj[3]);
        }
        fflush(stderr);
    }
    fprintf(stderr, "[host] 模块注册成功，init/generateHttpSignatureHeaders 已取得\n");

    /* 调用 init({deviceId}) —— data/this 必须与 Node 语义一致（this=exports 模块对象） */
    nv_value *arg_dev = nv_obj();
    nv_set(arg_dev, "deviceId", nv_str(device_id));
    nv_value *init_argv[1] = { arg_dev };
    struct napi_callback_info_s init_info = { 1, init_argv, (nv_value *)mod_obj, fn_init->u.fn.data };
    napi_value r_init = fn_init->u.fn.cb(env, &init_info);
    (void)r_init;
    if (g_exception_pending) {
        fprintf(stderr, "[host] init 异常: %s\n", g_last_error_msg);
        g_exception_pending = 0;
        if (g_last_exception) { nv_del(g_last_exception); g_last_exception = NULL; }
        return 1;
    }


    /* dumpmem 模式：argv[5]=="dumpmem" 输出路径，dump metasecml 运行时内存 */
    if (argc > 5 && strcmp(argv[5], "dumpmem") == 0) {
        const char *outpath = argc > 6 ? argv[6] : "Z:\\metasecml.dump";
        HMODULE hMeta = GetModuleHandleA("metasecml.dll");
        if (!hMeta) { fprintf(stderr, "[host] metasecml 未加载\n"); return 1; }
        IMAGE_DOS_HEADER *md = (IMAGE_DOS_HEADER *)hMeta;
        IMAGE_NT_HEADERS *mn = (IMAGE_NT_HEADERS *)((BYTE *)hMeta + md->e_lfanew);
        DWORD vsize = mn->OptionalHeader.SizeOfImage;
        fprintf(stderr, "[host] metasecml base=%p size=%u\n", (void *)hMeta, vsize);
        HANDLE hf = CreateFileA(outpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) { fprintf(stderr, "[host] 打开 %s 失败 %lu\n", outpath, GetLastError()); return 1; }
        DWORD wr = 0;
        WriteFile(hf, hMeta, vsize, &wr, NULL);
        CloseHandle(hf);
        fprintf(stderr, "[host] dump 完成 %lu 字节 -> %s\n", wr, outpath);
        return 0;
    }

    /* server 常驻模式：argv[5] == "server"，stdin 行协议
       输入行:  b64url<TAB>b64headers\n
       输出行:  b64signature\n  或 ERR:<msg>\n */
    if (argc > 5 && strcmp(argv[5], "server") == 0) {
        char line[32768];
        while (fgets(line, sizeof line, stdin)) {
            size_t llen = strlen(line);
            while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = 0;
            char *tab = strchr(line, '\t');
            if (!tab) { fprintf(stdout, "ERR:badline\n"); fflush(stdout); continue; }
            *tab = 0;
            size_t ulen = 0, hlen = 0;
            unsigned char *u = b64_decode(line, &ulen);
            unsigned char *h = b64_decode(tab + 1, &hlen);
            if (!u || !h) { fprintf(stdout, "ERR:b64\n"); fflush(stdout); if (u) free(u); if (h) free(h); continue; }
            nv_value *g_argv[2] = { nv_str((const char *)u), nv_str((const char *)h) };
            struct napi_callback_info_s g_info = { 2, g_argv, (nv_value *)mod_obj, fn_gen->u.fn.data };
            napi_value r_g = fn_gen->u.fn.cb(env, &g_info);
            if (g_exception_pending) {
                fprintf(stdout, "ERR:%s\n", g_last_error_msg);
                fflush(stdout);
                g_exception_pending = 0;
            } else if (!r_g || r_g->type != NV_STRING) {
                fprintf(stdout, "ERR:notstr\n"); fflush(stdout);
            } else {
                size_t olen = 0;
                char *ob = b64_encode((const unsigned char *)r_g->u.str, strlen(r_g->u.str), &olen);
                if (ob) { fprintf(stdout, "%s\n", ob); free(ob); }
                else fprintf(stdout, "ERR:enc\n");
                fflush(stdout);
            }
            free(u); free(h);
        }
        return 0;
    }

    /* 调用 generateHttpSignatureHeaders(url, headersLines)，argv[5]=循环次数（进程内基准） */
    int loops = argc > 5 ? atoi(argv[5]) : 1;
    if (loops < 1) loops = 1;
    nv_value *gen_argv[2] = { nv_str(url), nv_str(headers_lines) };
    struct napi_callback_info_s gen_info = { 2, gen_argv, (nv_value *)mod_obj, fn_gen->u.fn.data };
    LARGE_INTEGER qfreq, qt0, qt1;
    QueryPerformanceFrequency(&qfreq);
    QueryPerformanceCounter(&qt0);
    for (int li = 0; li < loops; li++) {
        napi_value r_gen = fn_gen->u.fn.cb(env, &gen_info);
        if (g_exception_pending) {
            fprintf(stderr, "[host] generate 异常: %s\n", g_last_error_msg);
            return 1;
        }
        if (li == loops - 1) {
            if (!r_gen || r_gen->type != NV_STRING) {
                fprintf(stderr, "[host] generate 返回非字符串\n");
                return 1;
            }
            /* 输出签名头（stdout，name\r\nvalue\r\n...） */
            fwrite(r_gen->u.str, 1, strlen(r_gen->u.str), stdout);
            fflush(stdout);
        }
    }
    QueryPerformanceCounter(&qt1);
    double ms = (double)(qt1.QuadPart - qt0.QuadPart) * 1000.0 / (double)qfreq.QuadPart;
    fprintf(stderr, "[bench] %d 次签名 %.2f ms，平均 %.3f ms/次 (%.1f QPS)\n",
        loops, ms, ms / loops, loops * 1000.0 / ms);
    fflush(stderr);
    return 0;
}

/*
 * Ionic ML model runtime
 *
 * Supported backends:
 *   .onnx              — ONNX Runtime (CoreML GPU on macOS)
 *   .gguf              — llama.cpp (Metal GPU on macOS)
 *   .pt / .pth         — stub (LibTorch not yet linked)
 *   .h5                — stub (Keras/HDF5 not yet linked)
 *   .mlmodel           — stub (CoreML direct load not yet linked)
 *
 * Public API:
 *   ionic_load_model(path)                                 -> opaque handle
 *   ionic_model_free(model)                                -> void
 *   ionic_model_forward(model, input)                      -> tensor ptr (ONNX single-input)
 *   ionic_piper_forward(model, phoneme_arr, ns, ls, nw)    -> float64 array
 *   ionic_write_wav(path, arr, n_samples, sample_rate)     -> void
 *   ionic_gguf_generate(model, prompt, max_tokens)         -> string (char*)
 *   ionic_gguf_set_temp(model, temp)                       -> void
 *   ionic_gguf_set_top_p(model, top_p)                     -> void
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ── ONNX Runtime ─────────────────────────────────────────────────────────── */
#ifdef IONIC_HAVE_ORT
#  include "onnxruntime_c_api.h"
#  ifdef __APPLE__
#    include "coreml_provider_factory.h"
#  endif
#endif

/* ── llama.cpp (GGUF) ─────────────────────────────────────────────────────── */
#ifdef IONIC_HAVE_LLAMA
#  include "llama.h"
#endif

/* ── Ionic dynamic-array layout ──────────────────────────────────────────────
 *   struct IonicArray { i64 len; i64 cap; void *data; }
 *   data is an array of i64 slots (8 bytes each)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int64_t len;
    int64_t cap;
    void   *data;
} IonicArray;

static IonicArray *ionic_array_alloc(int64_t cap) {
    IonicArray *a = (IonicArray *)malloc(sizeof(IonicArray));
    a->len  = 0;
    a->cap  = cap > 0 ? cap : 8;
    a->data = malloc((size_t)(a->cap * 8));
    return a;
}

static void ionic_array_push_i64(IonicArray *a, int64_t v) {
    if (a->len >= a->cap) {
        a->cap *= 2;
        a->data = realloc(a->data, (size_t)(a->cap * 8));
    }
    ((int64_t *)a->data)[a->len++] = v;
}

/* ── System runtime ───────────────────────────────────────────────────────── */

static int    ionic_argc = 0;
static char **ionic_argv = NULL;

char *ionic_fgets_stdin(char *buf, int n) {
    char *r = fgets(buf, n, stdin);
    if (!r) { buf[0] = '\0'; return buf; }
    /* Strip trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return buf;
}

/* Create a new empty array (for array-literal codegen). */
IonicArray *ionic_make_array(void) { return ionic_array_alloc(8); }

/* Write a string to a file (text mode). */
int64_t ionic_file_write(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content ? content : "", f);
    fclose(f);
    return 0;
}

/* Write a [int64] byte array as raw binary. Each element is one byte (0-255). */
int64_t ionic_file_write_binary(IonicArray *arr, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int64_t *data = (int64_t *)arr->data;
    for (int64_t i = 0; i < arr->len; i++) {
        uint8_t byte = (uint8_t)(data[i] & 0xFF);
        fwrite(&byte, 1, 1, f);
    }
    fclose(f);
    return arr->len;
}

int64_t ionic_arr_reset(IonicArray *arr) {
    arr->len = 0;
    return 0;
}

void ionic_runtime_init(int argc, char **argv) {
    ionic_argc = argc;
    ionic_argv = argv;
}

const char *ionic_get_arg(int64_t n) {
    int idx = (int)(n + 1);
    if (idx < 1 || idx >= ionic_argc || !ionic_argv[idx]) return "";
    return ionic_argv[idx];
}

/* Read an environment variable. Returns empty string (not NULL) when unset.
 * Used by the import resolver to locate the per-user package cache
 * (~/.ionic/packages on Unix; falls back to USERPROFILE on Windows). */
const char *ionic_getenv(const char *name) {
    if (!name) return "";
    const char *v = getenv(name);
    return v ? v : "";
}

int64_t ionic_target_is_linux(void) {
#ifdef __linux__
    return 1;
#else
    return 0;
#endif
}

int64_t ionic_cpu_core_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    return (int64_t)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int64_t)n : 1;
#endif
}

/* ── File I/O builtins ────────────────────────────────────────────────────── */

/* Marked weak: LLVM IR path defines ionic_file_read inline */
__attribute__((weak))
char *ionic_file_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { return (char *)""; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)(sz + 1));
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

int64_t ionic_system(const char *cmd) { return (int64_t)system(cmd); }

/* ── Basic I/O builtins ───────────────────────────────────────────────────── */

int64_t ionic_println(const char *s) { puts(s ? s : ""); return 0; }
int64_t ionic_println_int64(int64_t v) { printf("%lld\n", (long long)v); return 0; }
int64_t ionic_print(const char *s) { fputs(s ? s : "", stdout); return 0; }
int64_t ionic_print_int64(int64_t v) { printf("%lld", (long long)v); return 0; }

/* ── Math builtins ────────────────────────────────────────────────────────── */
/* All float functions use int64 bit-pattern convention: args and return values
   are the IEEE 754 bit representation passed through GP registers (X0/X0).
   This avoids the ARM64 float ABI (D0) mismatch in the native codegen. */
static inline int64_t d2i64(double v) { int64_t b; memcpy(&b, &v, 8); return b; }
static inline double  i642d(int64_t b) { double v; memcpy(&v, &b, 8); return v; }

int64_t ionic_sqrt(int64_t b) { return d2i64(sqrt(i642d(b))); }
int64_t ionic_fabs(int64_t b) { return d2i64(fabs(i642d(b))); }
int64_t ionic_int64_to_float64(int64_t v) { return d2i64((double)v); }
int64_t ionic_float64_to_int64(int64_t b) { return (int64_t)i642d(b); }

/* Compile-time float arithmetic — used by the optimizer to fold floats when
   ionic_new runs. Same int64 bit-pattern ABI as all other float builtins.
   Return type is int64 (the bit pattern) so the optimizer can store results
   directly into nd_iv of freshly-built ND_FLOAT nodes. These are plain-named
   (no ionic_ prefix) so call sites resolve identically through both the old
   bootstrap (ionic_self) and the self-hosted ionic_new. */
int64_t fadd_bits(int64_t a, int64_t b) { return d2i64(i642d(a) + i642d(b)); }
int64_t fsub_bits(int64_t a, int64_t b) { return d2i64(i642d(a) - i642d(b)); }
int64_t fmul_bits(int64_t a, int64_t b) { return d2i64(i642d(a) * i642d(b)); }
int64_t fdiv_bits(int64_t a, int64_t b) { return d2i64(i642d(a) / i642d(b)); }
int64_t sqrt_bits(int64_t b)            { return d2i64(sqrt(i642d(b))); }
int64_t fabs_bits(int64_t b)            { return d2i64(fabs(i642d(b))); }
int64_t floor_bits(int64_t b)           { return d2i64(floor(i642d(b))); }
int64_t ceil_bits(int64_t b)            { return d2i64(ceil(i642d(b))); }
int64_t pow_bits(int64_t a, int64_t b)  { return d2i64(pow(i642d(a), i642d(b))); }
int64_t int_to_fbits(int64_t v)         { return d2i64((double)v); }

/* ── Float literal parsing (used internally by the Ionic parser) ─────────── */
/* No ionic_ prefix — ionic_self generates _str_to_float64_bits directly */
int64_t str_to_float64_bits(const char *s) {
    if (!s || !*s) return 0;
    return d2i64(strtod(s, NULL));
}

/* ── Conversion & hashing ─────────────────────────────────────────────────── */

int64_t ionic_str_to_int64(const char *s) {
    if (!s || !*s) return 0;
    return (int64_t)strtoll(s, NULL, 10);
}

int64_t ionic_str_hash(const char *s) {
    if (!s) return 0;
    int64_t h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) ^ c;
    return h;
}

int64_t ionic_exit(int64_t code) { exit((int)code); return 0; }

int64_t ionic_min(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t ionic_max(int64_t a, int64_t b) { return a > b ? a : b; }
int64_t ionic_abs(int64_t v)            { return v < 0 ? -v : v; }
int64_t ionic_pow(int64_t ab, int64_t eb) { return d2i64(pow(i642d(ab), i642d(eb))); }
int64_t ionic_floor(int64_t b)          { return d2i64(floor(i642d(b))); }
int64_t ionic_ceil(int64_t b)           { return d2i64(ceil(i642d(b))); }

/* ── String utilities ─────────────────────────────────────────────────────── */

char *ionic_read_line(void) {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) { char *r = (char *)malloc(1); r[0] = '\0'; return r; }
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    char *r = (char *)malloc(strlen(buf) + 1);
    strcpy(r, buf);
    return r;
}

char *ionic_str_slice(const char *s, int64_t start, int64_t end) {
    if (!s) { char *r = (char *)malloc(1); r[0] = '\0'; return r; }
    int64_t n = (int64_t)strlen(s);
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start >= end) { char *r = (char *)malloc(1); r[0] = '\0'; return r; }
    int64_t len = end - start;
    char *r = (char *)malloc((size_t)(len + 1));
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}

int64_t ionic_str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL ? 1 : 0;
}

int64_t ionic_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return 0;
    return strncmp(s, prefix, strlen(prefix)) == 0 ? 1 : 0;
}

char *ionic_str_replace(const char *s, const char *from, const char *to) {
    if (!s || !from || !to) return (char *)(s ? s : "");
    size_t ls = strlen(s), lf = strlen(from), lt = strlen(to);
    if (lf == 0) { char *r = (char *)malloc(ls + 1); memcpy(r, s, ls + 1); return r; }
    /* Count occurrences */
    int count = 0;
    const char *p = s;
    while ((p = strstr(p, from))) { count++; p += lf; }
    size_t need = ls + (size_t)count * (lt > lf ? lt - lf : 0) + 1;
    char *r = (char *)malloc(need);
    char *out = r;
    p = s;
    const char *q;
    while ((q = strstr(p, from))) {
        size_t pre = (size_t)(q - p);
        memcpy(out, p, pre); out += pre;
        memcpy(out, to, lt); out += lt;
        p = q + lf;
    }
    size_t tail = strlen(p);
    memcpy(out, p, tail + 1);
    return r;
}

int64_t ionic_str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t ls = strlen(s), lp = strlen(suffix);
    if (lp > ls) return 0;
    return strcmp(s + ls - lp, suffix) == 0 ? 1 : 0;
}

/* ── String builtins ──────────────────────────────────────────────────────── */

/* These are also defined inline by the LLVM IR emitter; mark weak so the
   IR's strong definition wins when linking ionic_self via the LLVM path.
   For the native (ARM64) path there are no IR definitions, so these are used. */

__attribute__((weak))
char *ionic_str_concat(const char *a, const char *b) {
    if (!a) a = ""; if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *r = (char *)malloc(la + lb + 1);
    memcpy(r, a, la); memcpy(r + la, b, lb); r[la + lb] = '\0';
    return r;
}

int64_t ionic_str_eq(const char *a, const char *b) {
    if (!a) a = ""; if (!b) b = "";
    return strcmp(a, b) == 0 ? 1 : 0;
}

int64_t ionic_str_len(const char *s) {
    return s ? (int64_t)strlen(s) : 0;
}

int64_t ionic_str_index(const char *s, int64_t i) {
    if (!s || i < 0 || i >= (int64_t)strlen(s)) return 0;
    return (int64_t)(unsigned char)s[i];
}

__attribute__((weak))
char *ionic_int64_to_str(int64_t v) {
    char *buf = (char *)malloc(32);
    snprintf(buf, 32, "%lld", (long long)v);
    return buf;
}

__attribute__((weak))
char *ionic_float64_to_str(int64_t b) {
    double v = i642d(b);
    char *buf = (char *)malloc(32);
    snprintf(buf, 32, "%g", v);
    return buf;
}

int64_t ionic_println_float64(int64_t b) { printf("%g\n",  i642d(b)); fflush(stdout); return 0; }
int64_t ionic_print_float64(int64_t b)   { printf("%g",    i642d(b)); fflush(stdout); return 0; }

/* format("{} has {} items", s, int64_to_str(n)) — replaces {} placeholders in order */
char *ionic_format(const char *fmt,
    const char *a0, const char *a1, const char *a2, const char *a3,
    const char *a4, const char *a5, const char *a6) {
    const char *args[7] = {a0, a1, a2, a3, a4, a5, a6};
    /* first pass: compute output length */
    size_t out_sz = 0;
    int ai = 0;
    for (const char *p = fmt ? fmt : ""; *p; p++) {
        if (*p == '{' && *(p+1) == '}') {
            if (ai < 7 && args[ai]) out_sz += strlen(args[ai]);
            ai++; p++;
        } else {
            out_sz++;
        }
    }
    char *out = (char *)malloc(out_sz + 1);
    char *op = out;
    ai = 0;
    for (const char *p = fmt ? fmt : ""; *p; p++) {
        if (*p == '{' && *(p+1) == '}') {
            const char *a = (ai < 7 && args[ai]) ? args[ai] : "";
            size_t al = strlen(a);
            memcpy(op, a, al); op += al;
            ai++; p++;
        } else {
            *op++ = *p;
        }
    }
    *op = '\0';
    return out;
}

__attribute__((weak))
char *ionic_char_to_str(int64_t c) {
    char *buf = (char *)malloc(2);
    buf[0] = (char)(c & 0x7F); buf[1] = '\0';
    return buf;
}

/* ── Array builtins ───────────────────────────────────────────────────────── */

__attribute__((weak))
int64_t ionic_array_len(IonicArray *arr) { return arr ? arr->len : 0; }

static void ionic_bounds_panic(int64_t idx, int64_t len) {
    fprintf(stderr, "ionic: index out of bounds: index %lld, length %lld\n",
            (long long)idx, (long long)len);
    exit(1);
}

__attribute__((weak))
int64_t ionic_array_get(IonicArray *arr, int64_t i) {
    if (!arr) { fprintf(stderr, "ionic: index into null array\n"); exit(1); }
    if (i < 0 || i >= arr->len) ionic_bounds_panic(i, arr->len);
    return ((int64_t *)arr->data)[i];
}

__attribute__((weak))
int64_t ionic_array_set(IonicArray *arr, int64_t i, int64_t v) {
    if (!arr) { fprintf(stderr, "ionic: index into null array\n"); exit(1); }
    if (i < 0 || i >= arr->len) ionic_bounds_panic(i, arr->len);
    ((int64_t *)arr->data)[i] = v; return 0;
}

__attribute__((weak))
void ionic_array_push(IonicArray *arr, int64_t v) { ionic_array_push_i64(arr, v); }

/* ── Model format detection ───────────────────────────────────────────────── */

typedef enum {
    MODEL_FMT_UNKNOWN = 0,
    MODEL_FMT_ONNX,
    MODEL_FMT_GGUF,
    MODEL_FMT_PT,
    MODEL_FMT_H5,
    MODEL_FMT_MLMODEL,
} ModelFormat;

static ModelFormat detect_format(const char *path) {
    /* Extension check first */
    const char *dot = strrchr(path, '.');
    if (dot) {
        if (strcmp(dot, ".onnx")    == 0) return MODEL_FMT_ONNX;
        if (strcmp(dot, ".gguf")    == 0) return MODEL_FMT_GGUF;
        if (strcmp(dot, ".pt")      == 0) return MODEL_FMT_PT;
        if (strcmp(dot, ".pth")     == 0) return MODEL_FMT_PT;
        if (strcmp(dot, ".h5")      == 0) return MODEL_FMT_H5;
        if (strcmp(dot, ".mlmodel") == 0) return MODEL_FMT_MLMODEL;
    }
    /* Magic-byte fallback for extension-less files (e.g. Ollama blobs) */
    FILE *f = fopen(path, "rb");
    if (!f) return MODEL_FMT_UNKNOWN;
    unsigned char magic[8] = {0};
    fread(magic, 1, sizeof(magic), f);
    fclose(f);
    /* GGUF: "GGUF" */
    if (magic[0]=='G' && magic[1]=='G' && magic[2]=='U' && magic[3]=='F') return MODEL_FMT_GGUF;
    /* ONNX protobuf: starts with 0x08 field-tag (common for ModelProto) — heuristic only */
    /* ORT also has its own binary format; skip for now */
    return MODEL_FMT_UNKNOWN;
}

/* ════════════════════════════════════════════════════════════════════════════
 * ONNX Runtime backend
 * ════════════════════════════════════════════════════════════════════════════ */

#ifdef IONIC_HAVE_ORT

static const OrtApi *g_ort = NULL;

static void ort_check(OrtStatus *status, const char *ctx) {
    if (!status) return;
    const char *msg = g_ort->GetErrorMessage(status);
    fprintf(stderr, "[ionic/ort] %s: %s\n", ctx, msg);
    g_ort->ReleaseStatus(status);
    exit(1);
}

static void ensure_ort(void) {
    if (g_ort) return;
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) { fprintf(stderr, "[ionic/ort] API init failed\n"); exit(1); }
}

typedef struct {
    ModelFormat  fmt;       /* MODEL_FMT_ONNX */
    OrtEnv      *env;
    OrtSession  *session;
} OnnxHandle;

static void *ort_load(const char *path) {
    ensure_ort();
    OnnxHandle *h = calloc(1, sizeof(OnnxHandle));
    h->fmt = MODEL_FMT_ONNX;
    ort_check(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ionic", &h->env), "CreateEnv");
    OrtSessionOptions *opts = NULL;
    ort_check(g_ort->CreateSessionOptions(&opts), "CreateSessionOptions");
#ifdef __APPLE__
    OrtStatus *cml_st = OrtSessionOptionsAppendExecutionProvider_CoreML(opts, 0);
    if (cml_st) { g_ort->ReleaseStatus(cml_st); }
    else        { fprintf(stderr, "[ionic/ort] CoreML EP enabled\n"); }
#endif
    ort_check(g_ort->CreateSession(h->env, path, opts, &h->session), "CreateSession");
    g_ort->ReleaseSessionOptions(opts);
    fprintf(stderr, "[ionic/ort] loaded: %s\n", path);
    return h;
}

static void ort_free(void *model) {
    ensure_ort();
    OnnxHandle *h = model;
    if (h->session) g_ort->ReleaseSession(h->session);
    if (h->env)     g_ort->ReleaseEnv(h->env);
    free(h);
}

#endif /* IONIC_HAVE_ORT */

/* ════════════════════════════════════════════════════════════════════════════
 * llama.cpp / GGUF backend
 * ════════════════════════════════════════════════════════════════════════════ */

#ifdef IONIC_HAVE_LLAMA

static int g_llama_backend_init = 0;

static void ensure_llama_backend(void) {
    if (g_llama_backend_init) return;
    llama_backend_init();
    g_llama_backend_init = 1;
}

typedef struct {
    ModelFormat          fmt;        /* MODEL_FMT_GGUF */
    struct llama_model  *model;
    struct llama_context*ctx;
    float                temperature; /* sampling temperature */
    float                top_p;       /* nucleus sampling p   */
} GgufHandle;

static void *gguf_load(const char *path) {
    ensure_llama_backend();

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 999; /* offload all layers to Metal */

    struct llama_model *model = llama_model_load_from_file(path, mparams);
    if (!model) {
        fprintf(stderr, "[ionic/llama] failed to load: %s\n", path);
        return NULL;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx   = 4096;
    cparams.n_batch = 512;

    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "[ionic/llama] failed to create context\n");
        llama_model_free(model);
        return NULL;
    }

    GgufHandle *h = calloc(1, sizeof(GgufHandle));
    h->fmt         = MODEL_FMT_GGUF;
    h->model       = model;
    h->ctx         = ctx;
    h->temperature = 0.8f;
    h->top_p       = 0.95f;

    fprintf(stderr, "[ionic/llama] loaded: %s\n", path);
    return h;
}

static void ionic_gguf_handle_free(void *model) {
    GgufHandle *h = model;
    /* Order matters: free context before model, then backend to drain Metal residency sets */
    if (h->ctx)   { llama_free(h->ctx);         h->ctx   = NULL; }
    if (h->model) { llama_model_free(h->model);  h->model = NULL; }
    llama_backend_free();
    g_llama_backend_init = 0;
    free(h);
}

/* Generate text from a prompt; returns a malloc'd string owned by the runtime */
static const char *gguf_generate(void *model, const char *prompt, int64_t max_tokens) {
    GgufHandle *h = model;
    const struct llama_vocab *vocab = llama_model_get_vocab(h->model);

    /* Tokenise prompt */
    int n_prompt_tokens = -llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                           NULL, 0, 1, 1);
    llama_token *prompt_tokens = malloc((size_t)n_prompt_tokens * sizeof(llama_token));
    llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                   prompt_tokens, n_prompt_tokens, 1, 1);

    /* KV-cache reset */
    llama_memory_clear(llama_get_memory(h->ctx), 1);

    /* Build sampler chain */
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(h->top_p, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(h->temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    /* Decode prompt in one batch */
    struct llama_batch batch = llama_batch_get_one(prompt_tokens, n_prompt_tokens);
    if (llama_decode(h->ctx, batch) != 0) {
        fprintf(stderr, "[ionic/llama] prompt decode failed\n");
        llama_sampler_free(smpl);
        free(prompt_tokens);
        return "";
    }

    /* Accumulate output into a growable buffer */
    size_t buf_cap = 4096;
    size_t buf_len = 0;
    char  *buf     = malloc(buf_cap);
    buf[0] = '\0';

    char piece[256];
    int64_t generated = 0;

    while (generated < max_tokens) {
        llama_token tok = llama_sampler_sample(smpl, h->ctx, -1);

        if (llama_vocab_is_eog(vocab, tok)) break;

        int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece) - 1, 0, 1);
        if (n < 0) n = 0;
        piece[n] = '\0';

        /* Grow buffer if needed */
        while (buf_len + (size_t)n + 1 > buf_cap) {
            buf_cap *= 2;
            buf = realloc(buf, buf_cap);
        }
        memcpy(buf + buf_len, piece, (size_t)n);
        buf_len += (size_t)n;
        buf[buf_len] = '\0';

        /* Decode the new token */
        llama_token new_tok = tok;
        struct llama_batch next = llama_batch_get_one(&new_tok, 1);
        if (llama_decode(h->ctx, next) != 0) break;

        generated++;
    }

    llama_sampler_free(smpl);
    free(prompt_tokens);

    fprintf(stderr, "[ionic/llama] generated %lld tokens\n", (long long)generated);
    return buf; /* caller owns — but Ionic treats strings as const; leak is acceptable for now */
}

#endif /* IONIC_HAVE_LLAMA */

/* ════════════════════════════════════════════════════════════════════════════
 * Public API — dispatch on ModelFormat stored in handle
 * ════════════════════════════════════════════════════════════════════════════ */

/* The first field of every handle struct is ModelFormat fmt — safe to cast */
static ModelFormat handle_fmt(void *model) {
    if (!model) return MODEL_FMT_UNKNOWN;
    return *(ModelFormat *)model;
}

void *ionic_load_model(const char *path) {
    ModelFormat fmt = detect_format(path);
    switch (fmt) {
#ifdef IONIC_HAVE_ORT
        case MODEL_FMT_ONNX:
            return ort_load(path);
#endif
#ifdef IONIC_HAVE_LLAMA
        case MODEL_FMT_GGUF:
            return gguf_load(path);
#endif
        case MODEL_FMT_PT:
            fprintf(stderr, "[ionic] load_model: PyTorch (.pt/.pth) not yet supported\n");
            return NULL;
        case MODEL_FMT_H5:
            fprintf(stderr, "[ionic] load_model: Keras/HDF5 (.h5) not yet supported\n");
            return NULL;
        case MODEL_FMT_MLMODEL:
            fprintf(stderr, "[ionic] load_model: CoreML (.mlmodel) not yet supported\n");
            return NULL;
        default:
            fprintf(stderr, "[ionic] load_model: unknown format '%s'\n", path);
            return NULL;
    }
}

void ionic_model_free(void *model) {
    if (!model) return;
    switch (handle_fmt(model)) {
#ifdef IONIC_HAVE_ORT
        case MODEL_FMT_ONNX: ort_free(model); break;
#endif
#ifdef IONIC_HAVE_LLAMA
        case MODEL_FMT_GGUF: ionic_gguf_handle_free(model); break;
#endif
        default: free(model); break;
    }
}

void *ionic_model_forward(void *model, void *input) {
    if (!model) return input;
    fprintf(stderr, "[ionic] ionic_model_forward: use ionic_piper_forward for ONNX, ionic_gguf_generate for GGUF\n");
    return input;
}

/* ── GGUF text generation ─────────────────────────────────────────────────── */

const char *ionic_gguf_generate(void *model, const char *prompt, int64_t max_tokens) {
#ifdef IONIC_HAVE_LLAMA
    if (!model || handle_fmt(model) != MODEL_FMT_GGUF) {
        fprintf(stderr, "[ionic] gguf_generate: not a GGUF model\n");
        return "";
    }
    return gguf_generate(model, prompt, max_tokens);
#else
    (void)model; (void)prompt; (void)max_tokens;
    fprintf(stderr, "[ionic] gguf_generate: llama.cpp not compiled in\n");
    return "";
#endif
}

void ionic_gguf_set_temp(void *model, double temp) {
#ifdef IONIC_HAVE_LLAMA
    if (model && handle_fmt(model) == MODEL_FMT_GGUF)
        ((GgufHandle *)model)->temperature = (float)temp;
#else
    (void)model; (void)temp;
#endif
}

void ionic_gguf_set_top_p(void *model, double top_p) {
#ifdef IONIC_HAVE_LLAMA
    if (model && handle_fmt(model) == MODEL_FMT_GGUF)
        ((GgufHandle *)model)->top_p = (float)top_p;
#else
    (void)model; (void)top_p;
#endif
}

/* ── Piper TTS forward (ONNX) ─────────────────────────────────────────────── */

void *ionic_piper_forward(void *model,
                          void *phoneme_arr,
                          double noise_scale,
                          double length_scale,
                          double noise_w) {
#ifdef IONIC_HAVE_ORT
    ensure_ort();
    if (!model || handle_fmt(model) != MODEL_FMT_ONNX) {
        fprintf(stderr, "[ionic] piper_forward: not an ONNX model\n");
        return ionic_array_alloc(1);
    }

    OnnxHandle  *h   = model;
    IonicArray  *arr = phoneme_arr;
    int64_t      T   = arr->len;
    int64_t     *ids = (int64_t *)arr->data;

    OrtMemoryInfo *mem_info = NULL;
    ort_check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info),
              "CreateCpuMemoryInfo");

    int64_t shape_input[2] = {1, T};
    OrtValue *ov_input = NULL;
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, ids, (size_t)(T * sizeof(int64_t)),
        shape_input, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &ov_input), "input tensor");

    int64_t lengths_data[1] = {T};
    int64_t shape_len[1]    = {1};
    OrtValue *ov_lengths = NULL;
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, lengths_data, sizeof(int64_t),
        shape_len, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &ov_lengths), "lengths tensor");

    float scales_data[3] = {(float)noise_scale, (float)length_scale, (float)noise_w};
    int64_t shape_scales[1] = {3};
    OrtValue *ov_scales = NULL;
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(
        mem_info, scales_data, 3 * sizeof(float),
        shape_scales, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ov_scales), "scales tensor");

    const char *input_names[]  = {"input", "input_lengths", "scales"};
    const char *output_names[] = {"output"};
    OrtValue   *inputs[3]      = {ov_input, ov_lengths, ov_scales};
    OrtValue   *ov_output      = NULL;
    ort_check(g_ort->Run(h->session, NULL,
        input_names, (const OrtValue *const *)inputs, 3,
        output_names, 1, &ov_output), "Run");

    float *out_data = NULL;
    ort_check(g_ort->GetTensorMutableData(ov_output, (void **)&out_data), "GetTensorMutableData");

    OrtTensorTypeAndShapeInfo *shape_info = NULL;
    ort_check(g_ort->GetTensorTypeAndShape(ov_output, &shape_info), "GetTensorTypeAndShape");
    size_t elem_count = 0;
    ort_check(g_ort->GetTensorShapeElementCount(shape_info, &elem_count), "GetElementCount");
    g_ort->ReleaseTensorTypeAndShapeInfo(shape_info);

    IonicArray *result = ionic_array_alloc((int64_t)elem_count);
    for (size_t i = 0; i < elem_count; i++) {
        double d = (double)out_data[i];
        int64_t bits;
        memcpy(&bits, &d, 8);
        ionic_array_push_i64(result, bits);
    }

    fprintf(stderr, "[ionic/ort] piper: %zu samples\n", elem_count);

    g_ort->ReleaseValue(ov_output);
    g_ort->ReleaseValue(ov_scales);
    g_ort->ReleaseValue(ov_lengths);
    g_ort->ReleaseValue(ov_input);
    g_ort->ReleaseMemoryInfo(mem_info);
    return result;
#else
    (void)model; (void)phoneme_arr;
    (void)noise_scale; (void)length_scale; (void)noise_w;
    fprintf(stderr, "[ionic] piper_forward: ONNX Runtime not compiled in\n");
    return ionic_array_alloc(1);
#endif
}

/* ── WAV writer ───────────────────────────────────────────────────────────── */

static void write_le16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void write_le32(FILE *f, uint32_t v) {
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}

void ionic_write_wav(const char *path, void *samples_arr,
                     int64_t num_samples, int64_t sample_rate) {
    IonicArray *arr = samples_arr;
    int64_t     n   = (num_samples > 0 && num_samples <= arr->len) ? num_samples : arr->len;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[ionic] write_wav: cannot open '%s'\n", path); return; }

    uint32_t data_bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); write_le32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); write_le32(f, 16);
    write_le16(f, 1);                        /* PCM */
    write_le16(f, 1);                        /* mono */
    write_le32(f, (uint32_t)sample_rate);
    write_le32(f, (uint32_t)(sample_rate * 2)); /* byte rate */
    write_le16(f, 2);                        /* block align */
    write_le16(f, 16);                       /* bits per sample */
    fwrite("data", 1, 4, f); write_le32(f, data_bytes);

    int64_t *slots = arr->data;
    for (int64_t i = 0; i < n; i++) {
        double d; memcpy(&d, &slots[i], 8);
        if (d >  1.0) d =  1.0;
        if (d < -1.0) d = -1.0;
        int16_t s = (int16_t)(d * 32767.0);
        write_le16(f, (uint16_t)s);
    }
    fclose(f);
    fprintf(stderr, "[ionic] WAV: %s (%lld samples @ %lld Hz)\n",
            path, (long long)n, (long long)sample_rate);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Web request primitives
 *
 * A minimal dependency-free HTTP/1.1 client over POSIX sockets. Only the
 * blocking HTTP(S-free) path is implemented here; HTTPS would require TLS
 * (a separate dependency) and is stubbed to return an empty body.
 * ════════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <errno.h>
#endif

/*
 * Split a URL into host and port.
 *   "http://host:port/path"  -> host, port, rest (path)
 *   "https://host/..."       -> returns 0 (unsupported)
 */
static void url_parse(const char *url, char *host, int host_sz,
                      int *port, const char **path) {
    const char *h = url;
    *port = 80;
    *path = "/";

    if (strncmp(h, "http://", 7) == 0) { h += 7; }
    else if (strncmp(h, "https://", 8) == 0) {
        // TLS not implemented — mark port as unsupported by using -1
        *port = -1;
        h += 8;
    }

    /* host:port/path */
    const char *colon = strchr(h, ':');
    const char *slash = strchr(h, '/');
    const char *end = colon;
    if (end == NULL || (slash && slash < end)) end = slash;
    if (end == NULL) end = h + strlen(h);

    size_t hlen = (size_t)(end - h);
    if (hlen >= (size_t)host_sz) hlen = (size_t)(host_sz - 1);
    memcpy(host, h, hlen);
    host[hlen] = '\0';

    if (colon && (*colon == ':')) {
        *port = atoi(colon + 1);
        const char *p2 = strchr(colon + 1, '/');
        if (p2) *path = p2; else *path = "/";
    } else if (slash) {
        *path = slash;
    }
}

static void http_write_all(int sock, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(sock, buf + off, n - off, 0);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

/* Perform a request; returns malloc'd body (or "" on failure). */
static char *http_request(const char *method, const char *url,
                          const char *headers, const char *body) {
    char host[256];
    int  port = 80;
    const char *path = "/";
    url_parse(url, host, sizeof(host), &port, &path);
    if (port < 0) {
        fprintf(stderr, "[ionic/http] https not supported (no TLS): %s\n", url);
        char *e = (char *)malloc(1); e[0] = '\0'; return e;
    }

#ifdef _WIN32
    WSADATA ws; (void)ws;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { fprintf(stderr, "[ionic/http] socket: %s\n", strerror(errno)); return NULL; }

    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "[ionic/http] lookup failed: %s\n", host); close(sock); return NULL; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[ionic/http] connect: %s\n", strerror(errno));
        close(sock);
        return NULL;
    }

    /* Build request line + headers */
    char req[4096];
    {
        int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s\r\n",
                         method, path, host);
        if (n < 0) n = 0;
        if (n >= 0) {
            if (headers) {
                size_t hl = strlen(headers);
                if (n + (int)hl < (int)sizeof(req)) {
                    memcpy(req + n, headers, hl);
                    n += (int)hl;
                }
            }
            if (body) {
                char cl[64];
                snprintf(cl, sizeof(cl), "Content-Length: %lld\r\n", (long long)strlen(body));
                size_t cll = strlen(cl);
                if (n + (int)cll < (int)sizeof(req)) {
                    memcpy(req + n, cl, cll);
                    n += (int)cll;
                }
            }
            /* Ask the server to close after the response so we can read to EOF. */
            const char *closehdr = "Connection: close\r\n";
            size_t chl = strlen(closehdr);
            if (n + (int)chl < (int)sizeof(req)) {
                memcpy(req + n, closehdr, chl);
                n += (int)chl;
            }
            if (n + 2 < (int)sizeof(req)) {
                memcpy(req + n, "\r\n", 2); n += 2;
            }
            http_write_all(sock, req, n);
            if (body) http_write_all(sock, body, strlen(body));
        }
    }

    /* Read the full response into a growing buffer.
     * Return the ENTIRE raw response (status line + headers + body) so that
     * ionic_http_status / ionic_http_body can parse it. */
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(sock); return NULL; }
    char tmp[4096];
    for (;;) {
        ssize_t r = recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        if (len + (size_t)r + 1 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    buf[len] = '\0';
    close(sock);
    return buf;
#endif
}

char *ionic_http_get(const char *url) {
    char *b = http_request("GET", url, "", NULL);
    return b ? b : ((char *)"");
}

char *ionic_http_post(const char *url, const char *body) {
    char *b = http_request("POST", url, "Content-Type: application/json\r\n", body);
    return b ? b : ((char *)"");
}

/*
 * Return the HTTP status code from a full raw response. Callers that want the
 * code call this on the raw response string (not the body).
 */
int64_t ionic_http_status(const char *raw) {
    if (strncmp(raw, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(raw, ' ');
    if (!sp) return -1;
    return atoi(sp + 1);
}

/* Split a full HTTP response into headers and body. Returns the body. */
char *ionic_http_body(const char *raw) {
    const char *sep = strstr(raw, "\r\n\r\n");
    if (!sep) return (char *)(raw ? raw : "");
    char *b = (char *)malloc(strlen(sep + 4) + 1);
    strcpy(b, sep + 4);
    return b;
}

/* URL-encode a string (percent-encoding). */
char *ionic_http_urlencode(const char *s) {
    if (!s) s = "";
    char *out = (char *)malloc(strlen(s) * 3 + 1);
    char *p = out;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        unsigned char ch = *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            *p++ = (char)ch;
        } else {
            *p++ = '%'; *p++ = hex[(ch >> 4) & 0xF]; *p++ = hex[ch & 0xF];
        }
    }
    *p = '\0';
    return out;
}

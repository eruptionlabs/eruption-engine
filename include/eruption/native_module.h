/*
 * Eruption Engine: native game module (C ABI).
 *
 * C++ code of a game project lives in <project>/native (.cpp files). In the editor
 * (desktop) the engine compiles it into a shared library and reloads it while
 * the game runs whenever a file is saved; a build error keeps the previous
 * version running and shows the compiler message in the console. The folder
 * may hold .cpp/.cc and .c files, headers (native/ and native/include are on the
 * include path) and native/link.txt (one system library per line, e.g. ws2_32,
 * bcrypt: passed as -l<name> on the desktop build and linked into the game on
 * Android). An exported desktop game loads the last build. On Android the
 * sources are linked into the game library by tools/android/build_apk.sh
 * (ERUPTION_APK_NATIVE; Android permissions: native/android_permissions.txt or
 * ERUPTION_APK_PERMISSIONS). Unload and stop also run when the game exits.
 *
 * The module exports exactly one function, eruption_module(), returning a
 * description of itself. Everything the module keeps between frames goes in
 * host->state: memory owned by the engine that survives a reload (the module's
 * own globals do not). Changing the layout of that state is the module's job:
 * `reloaded` tells load() it is getting the previous bytes back.
 *
 * Scripts call the functions the module exposes as native.<name>(...), with
 * numbers in and one number out (API 1), or with any EruptionValue in and out
 * (API 2: expose_value). A module can also push events to the scripts with
 * host->emit; the scripts listen with native.on("event", function(value) ... end).
 *
 * Threads. Everything the host offers except emit must be called from the
 * engine's thread (load/unload/start/stop/update and the exposed functions run
 * there). emit is thread-safe: it copies the value into a queue that the engine
 * drains once per frame, on its thread, right after update(). The simplest
 * module polls its non-blocking sockets inside update().
 *
 * Android: the module's sources are linked into the game library (no
 * hot reload, no dlopen); the code is the same.
 */
#ifndef ERUPTION_NATIVE_MODULE_H
#define ERUPTION_NATIVE_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine loads modules built for API 1 or 2 (2 only adds to 1). */
#define ERUPTION_NATIVE_API_VERSION 2u

#if defined(_WIN32)
#define ERUPTION_MODULE_EXPORT __declspec(dllexport)
#else
#define ERUPTION_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct EruptionHost EruptionHost;

/* A value that crosses between the module and the scripts. nil, bool, number
   and string map to the Luau types; bytes <-> buffer; a table is a list of
   key/value pairs (key: string or number; no cycles, depth <= 16). The memory
   a value points to is owned by whoever built it and must stay valid until the
   call that received it returns (the engine copies what it keeps). */
typedef enum EruptionValueType {
    ERUPTION_NIL = 0,
    ERUPTION_BOOL = 1,
    ERUPTION_NUMBER = 2,
    ERUPTION_STRING = 3,
    ERUPTION_BYTES = 4,
    ERUPTION_TABLE = 5
} EruptionValueType;

typedef struct EruptionValue EruptionValue;
typedef struct EruptionPair EruptionPair;

struct EruptionValue {
    uint32_t type;           /* EruptionValueType */
    double number;           /* ERUPTION_NUMBER; ERUPTION_BOOL: 0 or 1 */
    const char* data;        /* ERUPTION_STRING / ERUPTION_BYTES */
    size_t size;             /* bytes of data */
    const EruptionPair* pairs; /* ERUPTION_TABLE */
    size_t count;            /* number of pairs */
};

struct EruptionPair {
    EruptionValue key;
    EruptionValue value;
};

static inline EruptionValue eruption_nil(void) {
    EruptionValue v = {ERUPTION_NIL, 0.0, NULL, 0, NULL, 0};
    return v;
}
static inline EruptionValue eruption_bool(int b) {
    EruptionValue v = {ERUPTION_BOOL, b ? 1.0 : 0.0, NULL, 0, NULL, 0};
    return v;
}
static inline EruptionValue eruption_number(double n) {
    EruptionValue v = {ERUPTION_NUMBER, n, NULL, 0, NULL, 0};
    return v;
}
static inline EruptionValue eruption_string_n(const char* s, size_t n) {
    EruptionValue v = {ERUPTION_STRING, 0.0, s, n, NULL, 0};
    return v;
}
static inline EruptionValue eruption_string(const char* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return eruption_string_n(s, n);
}
static inline EruptionValue eruption_bytes(const void* p, size_t n) {
    EruptionValue v = {ERUPTION_BYTES, 0.0, (const char*)p, n, NULL, 0};
    return v;
}
static inline EruptionValue eruption_table(const EruptionPair* pairs, size_t count) {
    EruptionValue v = {ERUPTION_TABLE, 0.0, NULL, 0, pairs, count};
    return v;
}

/* A function scripts can call: native.<name>(args...) -> number. It gets the
   host (and so the state) like the other callbacks. */
typedef double (*EruptionNativeFn)(EruptionHost* host, const double* args, int count);

/* API 2: a script-callable function with values in and one value out. `out`
   arrives as nil; leave it so to return nothing. What `out` points to must stay
   valid until the function returns (a static or stack buffer is fine). */
typedef void (*EruptionNativeValueFn)(EruptionHost* host, const EruptionValue* args, int count, EruptionValue* out);

/* What the engine offers the module. Valid only during the call it is passed to. */
struct EruptionHost {
    uint32_t version;    /* ERUPTION_NATIVE_API_VERSION */
    void* state;         /* zeroed on the first load; kept across reloads */
    size_t stateSize;    /* bytes in state (the module's stateSize) */

    /* Prints to the editor console. */
    void (*log)(const char* text);
    /* Makes fn callable from scripts as native.<name>(...). Call it in load():
       after a reload the old pointers are gone and the names are registered again. */
    void (*expose)(const char* name, EruptionNativeFn fn);

    /* Models of the open map, by name or index from 1 (0 = not found). */
    int (*findModel)(const char* name);
    void (*modelPosition)(int model, float out[3]);
    void (*setModelPosition)(int model, const float position[3]);
    /* Player position and the ground height under a point of the world. */
    void (*playerPosition)(float out[3]);
    float (*groundHeight)(float x, float z);
    /* Seconds since the game started (Play, in the editor). */
    double (*time)(void);

    /* API 2. Makes fn callable as native.<name>(...) with any values (and as
       native.call("<name>", ...)). Same rules as expose; wins over expose. */
    void (*expose_value)(const char* name, EruptionNativeValueFn fn);
    /* Sends an event to the scripts (native.on). Thread-safe; the value is
       copied. At most 4096 events wait for the next frame; extra ones are
       dropped with a warning. Returns 1 if queued, 0 if dropped. */
    int (*emit)(const char* event, const EruptionValue* value);
};

/* What the module tells the engine. Any callback may be NULL. */
typedef struct EruptionModule {
    uint32_t version;    /* ERUPTION_NATIVE_API_VERSION */
    size_t stateSize;    /* bytes the engine keeps for host->state */
    /* After loading. reloaded = 1 when state holds the previous version's bytes. */
    void (*load)(EruptionHost* host, int reloaded);
    /* Before unloading. reloading = 1 when a new build is about to replace it. */
    void (*unload)(EruptionHost* host, int reloading);
    /* The game starts (Play in the editor) and stops. */
    void (*start)(EruptionHost* host);
    void (*stop)(EruptionHost* host);
    /* Every frame while the game runs, before the scripts. */
    void (*update)(EruptionHost* host, float dt);
} EruptionModule;

/* The one symbol the module exports. */
ERUPTION_MODULE_EXPORT const EruptionModule* eruption_module(void);

#ifdef __cplusplus
}
#endif

#endif /* ERUPTION_NATIVE_MODULE_H */

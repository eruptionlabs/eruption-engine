/*
 * Eruption Engine: native game module (C ABI).
 *
 * C++ code of a game project lives in <project>/native (.cpp files). In the editor
 * (desktop) the engine compiles it into a shared library and reloads it while
 * the game runs whenever a file is saved; a build error keeps the previous
 * version running and shows the compiler message in the console. An exported
 * desktop game loads the last build. Mobile exports do not load modules yet
 * (planned: the same code linked into the game).
 *
 * The module exports exactly one function, eruption_module(), returning a
 * description of itself. Everything the module keeps between frames goes in
 * host->state: memory owned by the engine that survives a reload (the module's
 * own globals do not). Changing the layout of that state is the module's job:
 * `reloaded` tells load() it is getting the previous bytes back.
 *
 * Scripts call the functions the module exposes as native.<name>(...), with
 * numbers in and one number out.
 */
#ifndef ERUPTION_NATIVE_MODULE_H
#define ERUPTION_NATIVE_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ERUPTION_NATIVE_API_VERSION 1u

#if defined(_WIN32)
#define ERUPTION_MODULE_EXPORT __declspec(dllexport)
#else
#define ERUPTION_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct EruptionHost EruptionHost;

/* A function scripts can call: native.<name>(args...) -> number. It gets the
   host (and so the state) like the other callbacks. */
typedef double (*EruptionNativeFn)(EruptionHost* host, const double* args, int count);

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

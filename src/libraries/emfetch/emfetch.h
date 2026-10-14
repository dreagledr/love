/**
 * emfetch - minimal HTTP(S) API for LÖVE on Emscripten (love.js).
 *
 * Wraps the Emscripten Fetch API (emscripten_fetch) and exposes an asynchronous
 * Lua module named "http". Results are delivered through a callback queue that
 * must be pumped from love.update via http.poll().
 */

#ifndef LOVE_EMFETCH_EMFETCH_H
#define LOVE_EMFETCH_EMFETCH_H

// LOVE (pulls in the Lua C API wrapped in extern "C")
#include "common/runtime.h"

int luaopen_emfetch(lua_State *L);

#endif // LOVE_EMFETCH_EMFETCH_H

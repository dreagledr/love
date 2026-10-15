Adapting LÖVE to suit Emscripten. Would be neat if this was part of the official pipeline at some point in the future.

The final frontier, so to speak!

## emscripten-fetch

This fork is based on [Davidobot/love@emscripten](https://github.com/Davidobot/love/tree/emscripten)
and targets **LÖVE 11.5**. Everything interesting lives on the `emscripten-fetch`
branch (the default one).

On top of the Emscripten port it adds **emfetch** — a small asynchronous HTTP(S)
client built on the [Emscripten Fetch API](https://emscripten.org/docs/api_reference/fetch.html)
(`emscripten_fetch`, compiled with `-s FETCH=1`). It preloads a Lua module named
`http`, and only on Emscripten builds:

```lua
local http = require("http")

local id = http.request{
    url = "/api/random",
    method = "GET",                    -- GET/HEAD/POST/PUT/DELETE/PATCH
    headers = { ["Accept"] = "application/json" },
    data = nil,                        -- request body
    timeout = 10,                      -- seconds
    withCredentials = false,           -- send cookies
    onSuccess = function(res) end,     -- res = { status, statusText, body, headers }
    onError   = function(err) end,     -- err = { status, statusText, body, headers, message }
}

-- results are delivered from the callback queue, pumped once per frame:
http.poll()

-- optional:
http.cancel(id)
```

`onSuccess` fires for 2xx, `onError` for everything else (with the real status
code) and for transport errors. Because it is a real engine module there is no
JavaScript bridge, no `eval` and no sandbox escape.

Sources: [`src/libraries/emfetch`](src/libraries/emfetch). It is wired up in
[`src/modules/love/love.cpp`](src/modules/love/love.cpp) (`luax_preload(..., "http")`).

## Related repositories

* [dreagledr/megasource](https://github.com/dreagledr/megasource) — build
  aggregation, branch `emscripten-fetch`.
* [dreagledr/love.js](https://github.com/dreagledr/love.js) — the web tooling,
  prebuilt engine binaries and a runnable
  [fetch demo](https://github.com/dreagledr/love.js/tree/master/sample).

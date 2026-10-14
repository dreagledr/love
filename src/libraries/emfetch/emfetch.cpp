/**
 * emfetch - minimal HTTP(S) API for LÖVE on Emscripten (love.js).
 *
 * Wraps the Emscripten Fetch API (emscripten_fetch). The Lua module is named
 * "http" and is only preloaded on Emscripten builds.
 *
 * Lua API:
 *   local http = require("http")
 *   local id = http.request{
 *       url = "https://...",
 *       method = "GET",                     -- GET/HEAD/POST/PUT/DELETE/PATCH
 *       headers = { ["Key"] = "Value" },
 *       data = "body",
 *       timeout = 15,                       -- seconds
 *       withCredentials = true,             -- send cookies
 *       onSuccess = function(res) end,      -- res = {status, statusText, body, headers}
 *       onError   = function(err) end,      -- err = {status, statusText, body, headers, message}
 *   }
 *   http.poll()      -- call from love.update
 *   http.cancel(id)  -- optional
 */

#include "emfetch.h"

#ifdef __EMSCRIPTEN__

#include <emscripten/fetch.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{

struct Request
{
	int id = 0;
	int onSuccess = LUA_NOREF;
	int onError = LUA_NOREF;

	std::string url;
	std::string method;
	std::string body;

	// Request headers must stay alive for the whole (async) fetch operation.
	std::vector<std::string> headerStrings;
	std::vector<const char *> headerPtrs;

	emscripten_fetch_t *fetch = nullptr;

	bool cancelled = false;
	bool failed = false;

	unsigned short status = 0;
	std::string statusText;
	std::string responseBody;
	std::map<std::string, std::string> responseHeaders;
};

int g_nextId = 1;
std::vector<Request *> g_inflight;
std::vector<Request *> g_ready;

void parseHeadersString(const std::string &raw, std::map<std::string, std::string> &out)
{
	size_t pos = 0;
	while (pos < raw.size())
	{
		size_t eol = raw.find('\n', pos);
		std::string line = raw.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
		pos = (eol == std::string::npos) ? raw.size() : eol + 1;

		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);

		size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;

		std::string key = line.substr(0, colon);
		std::string value = line.substr(colon + 1);

		size_t first = value.find_first_not_of(" \t");
		size_t last = value.find_last_not_of(" \t");
		value = (first == std::string::npos) ? std::string() : value.substr(first, last - first + 1);

		if (!key.empty())
			out[key] = value;
	}
}

void collectResponse(emscripten_fetch_t *fetch, Request *r)
{
	r->status = fetch->status;
	r->statusText = fetch->statusText;

	if (fetch->data != nullptr && fetch->numBytes > 0)
		r->responseBody.assign(fetch->data, (size_t) fetch->numBytes);

	size_t headersLength = emscripten_fetch_get_response_headers_length(fetch);
	if (headersLength > 0)
	{
		std::vector<char> buffer(headersLength + 1, '\0');
		emscripten_fetch_get_response_headers(fetch, buffer.data(), buffer.size());
		parseHeadersString(std::string(buffer.data(), headersLength), r->responseHeaders);
	}
}

void emfetchOnSuccess(emscripten_fetch_t *fetch)
{
	Request *r = static_cast<Request *>(fetch->userData);
	if (r == nullptr || r->cancelled)
		return;

	collectResponse(fetch, r);
	g_ready.push_back(r);
}

void emfetchOnError(emscripten_fetch_t *fetch)
{
	Request *r = static_cast<Request *>(fetch->userData);
	if (r == nullptr || r->cancelled)
		return;

	collectResponse(fetch, r);
	r->failed = true;
	g_ready.push_back(r);
}

void removeInflight(Request *r)
{
	for (size_t i = 0; i < g_inflight.size(); ++i)
	{
		if (g_inflight[i] == r)
		{
			g_inflight.erase(g_inflight.begin() + i);
			return;
		}
	}
}

void unrefCallbacks(lua_State *L, Request *r)
{
	if (r->onSuccess != LUA_NOREF && r->onSuccess != LUA_REFNIL)
		luaL_unref(L, LUA_REGISTRYINDEX, r->onSuccess);
	if (r->onError != LUA_NOREF && r->onError != LUA_REFNIL)
		luaL_unref(L, LUA_REGISTRYINDEX, r->onError);

	r->onSuccess = LUA_NOREF;
	r->onError = LUA_NOREF;
}

int emfetch_request(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	Request *r = new Request();

	lua_getfield(L, 1, "url");
	if (!lua_isstring(L, -1))
	{
		lua_pop(L, 1);
		delete r;
		return luaL_error(L, "http.request: field 'url' (string) is required");
	}
	r->url = lua_tostring(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, 1, "method");
	r->method = lua_isstring(L, -1) ? lua_tostring(L, -1) : "GET";
	lua_pop(L, 1);

	lua_getfield(L, 1, "data");
	if (lua_isstring(L, -1))
	{
		size_t length = 0;
		const char *data = lua_tolstring(L, -1, &length);
		r->body.assign(data, length);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "headers");
	if (lua_istable(L, -1))
	{
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			if (lua_isstring(L, -2) && lua_isstring(L, -1))
			{
				size_t keyLength = 0, valueLength = 0;
				const char *key = lua_tolstring(L, -2, &keyLength);
				const char *value = lua_tolstring(L, -1, &valueLength);
				r->headerStrings.emplace_back(key, keyLength);
				r->headerStrings.emplace_back(value, valueLength);
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	for (const std::string &header : r->headerStrings)
		r->headerPtrs.push_back(header.c_str());
	r->headerPtrs.push_back(nullptr);

	double timeoutSeconds = 0.0;
	lua_getfield(L, 1, "timeout");
	if (lua_isnumber(L, -1))
		timeoutSeconds = lua_tonumber(L, -1);
	lua_pop(L, 1);

	bool withCredentials = false;
	lua_getfield(L, 1, "withCredentials");
	withCredentials = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);

	lua_getfield(L, 1, "onSuccess");
	if (lua_isfunction(L, -1))
		r->onSuccess = luaL_ref(L, LUA_REGISTRYINDEX);
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "onError");
	if (lua_isfunction(L, -1))
		r->onError = luaL_ref(L, LUA_REGISTRYINDEX);
	else
		lua_pop(L, 1);

	r->id = g_nextId++;
	lua_getfield(L, 1, "id");
	if (lua_isnumber(L, -1))
		r->id = (int) lua_tointeger(L, -1);
	lua_pop(L, 1);

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	std::strncpy(attr.requestMethod, r->method.c_str(), sizeof(attr.requestMethod) - 1);
	attr.requestMethod[sizeof(attr.requestMethod) - 1] = '\0';
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.onsuccess = emfetchOnSuccess;
	attr.onerror = emfetchOnError;
	attr.userData = r;
	attr.withCredentials = withCredentials ? 1 : 0;
	if (timeoutSeconds > 0.0)
		attr.timeoutMSecs = (unsigned long) (timeoutSeconds * 1000.0);
	if (!r->headerPtrs.empty())
		attr.requestHeaders = r->headerPtrs.data();
	if (!r->body.empty())
	{
		attr.requestData = r->body.data();
		attr.requestDataSize = r->body.size();
	}

	r->fetch = emscripten_fetch(&attr, r->url.c_str());
	if (r->fetch == nullptr)
	{
		unrefCallbacks(L, r);
		delete r;
		lua_pushnil(L);
		lua_pushliteral(L, "emscripten_fetch() failed to start");
		return 2;
	}

	g_inflight.push_back(r);

	lua_pushinteger(L, r->id);
	return 1;
}

int emfetch_poll(lua_State *L)
{
	// Swap out the ready queue so callbacks may safely enqueue new requests.
	std::vector<Request *> ready;
	ready.swap(g_ready);

	for (Request *r : ready)
	{
		int ref = r->failed ? r->onError : r->onSuccess;

		if (r->cancelled)
			ref = LUA_NOREF;

		if (ref != LUA_NOREF && ref != LUA_REFNIL)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
			lua_newtable(L);

			lua_pushinteger(L, (lua_Integer) r->status);
			lua_setfield(L, -2, "status");

			lua_pushlstring(L, r->statusText.data(), r->statusText.size());
			lua_setfield(L, -2, "statusText");

			lua_pushlstring(L, r->responseBody.data(), r->responseBody.size());
			lua_setfield(L, -2, "body");

			lua_newtable(L);
			for (const auto &header : r->responseHeaders)
			{
				lua_pushlstring(L, header.first.data(), header.first.size());
				lua_pushlstring(L, header.second.data(), header.second.size());
				lua_settable(L, -3);
			}
			lua_setfield(L, -2, "headers");

			if (r->failed)
			{
				lua_pushlstring(L, r->statusText.data(), r->statusText.size());
				lua_setfield(L, -2, "message");
			}

			if (lua_pcall(L, 1, 0, 0) != 0)
				lua_pop(L, 1);
		}

		unrefCallbacks(L, r);
		if (r->fetch != nullptr)
			emscripten_fetch_close(r->fetch);
		removeInflight(r);
		delete r;
	}

	return 0;
}

int emfetch_cancel(lua_State *L)
{
	int id = (int) luaL_checkinteger(L, 1);

	for (size_t i = 0; i < g_inflight.size(); ++i)
	{
		Request *r = g_inflight[i];
		if (r->id != id)
			continue;

		r->cancelled = true;
		unrefCallbacks(L, r);
		if (r->fetch != nullptr)
			emscripten_fetch_close(r->fetch); // may invoke onerror, which now bails out
		g_inflight.erase(g_inflight.begin() + i);
		delete r;

		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushboolean(L, 0);
	return 1;
}

} // namespace

int luaopen_emfetch(lua_State *L)
{
	lua_newtable(L);

	lua_pushcfunction(L, emfetch_request);
	lua_setfield(L, -2, "request");

	lua_pushcfunction(L, emfetch_poll);
	lua_setfield(L, -2, "poll");

	lua_pushcfunction(L, emfetch_cancel);
	lua_setfield(L, -2, "cancel");

	return 1;
}

#else // !__EMSCRIPTEN__

int luaopen_emfetch(lua_State *L)
{
	// Native builds don't preload this module; keep the symbol for completeness.
	lua_newtable(L);
	return 1;
}

#endif

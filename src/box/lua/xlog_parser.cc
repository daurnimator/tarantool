/*
 *
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "xlog_parser.h"
#include "msgpuck/msgpuck.h"

#include <box/xlog.h>
#include <box/xrow.h>
#include <ctype.h>
#include <box/iproto_constants.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include "lua/utils.h"

static int
parse_mp(struct lua_State *L, const char **beg)
{
	uint32_t size;
	switch(mp_typeof(**beg)) {
	case MP_UINT:
		lua_pushinteger(L, mp_decode_uint(beg));
		break;
	case MP_STR:
		const char *buf;
		buf = mp_decode_str(beg, &size);
		lua_pushlstring(L, buf, size);
		break;
	case MP_ARRAY:
		size = mp_decode_array(beg);
		lua_newtable(L);
		for(uint32_t i = 0; i < size; i++) {
			parse_mp(L, beg);
			lua_rawseti(L, -2, i + 1);
		}
		break;
	case MP_MAP:
		size = mp_decode_map(beg);
		lua_newtable(L);
		for (uint32_t i = 0; i < size; i++) {
			parse_mp(L, beg);
			parse_mp(L, beg);
			lua_settable(L, -3);
		}
		break;
	case MP_BOOL:
		lua_pushboolean(L, mp_decode_bool(beg));
		break;
	case MP_FLOAT:
		lua_pushnumber(L, mp_decode_float(beg));
		break;
	case MP_DOUBLE:
		lua_pushnumber(L, mp_decode_double(beg));
		break;
	case MP_INT:
		lua_pushinteger(L, mp_decode_int(beg));
		break;
	default:
		{
			char buf[32];
			sprintf(buf, "UNKNOWN MP_TYPE:%u", mp_typeof(**beg));
			lua_pushstring(L, buf);
			return -1;
		}
		break;
	}
	return 0;
}

static int
parse_body(struct lua_State *L, const char *ptr)
{
	const char **beg = &ptr;
	if (mp_typeof(**beg) == MP_MAP) {
		uint32_t size = mp_decode_map(beg);
		for (uint32_t i = 0; i < size; i++) {
			if (mp_typeof(**beg) == MP_UINT) {
				char buf[32];
				uint32_t v = mp_decode_uint(beg);
				if (v < IPROTO_KEY_MAX && iproto_key_strs[v] &&
						iproto_key_strs[v][0])
					sprintf(buf, "%s", iproto_key_strs[v]);
				else
					sprintf(buf, "unknown_key#%u", v);
				lua_pushstring(L, buf);
			}
			parse_mp(L, beg);
			lua_settable(L, -3);
		}
	}
	return 0;
}

static int
lbox_xlog_parser_close(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.iterate(xlog_pointer)");
	xlog_close(plog);
	return 0;
}

static int
lbox_xlog_parser_open(struct lua_State *L)
{

	int args_n = lua_gettop(L);
	if (args_n != 1 || !lua_isstring(L, 1)) {
usage:
		luaL_error(L, "Usage: parser.open(\"path/to/logs/"
			      "log_number(.xlog/.snap)\")");
	}
	const char *fname = luaL_checkstring(L, 1);
	xdir dir;
	xdir_type log_type;
	const int ext_len = 5;
	if (strlen(fname) < ext_len)
		goto usage;
	if (strncmp(fname + strlen(fname) - ext_len, ".xlog", ext_len) == 0)
		log_type = XLOG;
	else if(strncmp(fname + strlen(fname) - ext_len, ".snap", ext_len) == 0)
		log_type = SNAP;
	else
		goto usage;
	int i = strlen(fname) - 1;
	while (fname[i] != '.')
		i--;
	int fname_end = i;
	i--;
	while (i >= 0 && fname[i] != '/')
		i--;

	char buf[MAX(i + 1, fname_end - i)];

	memcpy(buf, fname + i + 1, fname_end - i - 1);
	buf[fname_end - i - 1] = 0;
	long long log_number = -1;
	sscanf(buf, "%lld", &log_number);
	if (log_number == -1)
		luaL_error(L, "filename must be nubmer");
	if (i < 0) {
		buf[0] = '.';
		buf[1] = 0;
	} else {
		memcpy(buf, fname, i + 1);
		buf[i] = 0;
	}
	tt_uuid  uuid_zero;
	memset(&uuid_zero, 0, sizeof(uuid_zero));
	try {
		xdir_create(&dir, buf, log_type, &uuid_zero);
	} catch(...) {
		printf("Exception\n");
		return 0;
	}
	xlog *plog = xlog_open(&dir, (int64_t)log_number);
	lua_pushlightuserdata(L, plog);
	return 1;
}

static int
next_row(struct lua_State *L, struct xlog_cursor *cur,
	 struct xrow_header *row) {

	if (xlog_cursor_next(cur, row) != 0)
		return -1;
	lua_newtable(L);
	lua_pushstring(L, "HEADER");

	lua_newtable(L);
	lua_pushstring(L, "type");
	if (row->type < IPROTO_TYPE_STAT_MAX && iproto_type_strs[row->type]) {
		lua_pushstring(L, iproto_type_strs[row->type]);
	} else {
		char buf[32];
		sprintf(buf, "UNKNOWN#%u", row->type);
		lua_pushstring(L, buf);
	}
	lua_settable(L, -3);
	lua_pushstring(L, "lsn");
	lua_pushinteger(L, row->lsn);
	lua_settable(L, -3);
	lua_pushstring(L, "server_id");
	lua_pushinteger(L, row->server_id);
	lua_settable(L, -3);
	lua_pushstring(L, "timestamp");
	lua_pushnumber(L, row->tm);
	lua_settable(L, -3);

	lua_settable(L, -3); /* HEADER */

	for (int i = 0; i < row->bodycnt; i++) {
		if (i == 0) {
			lua_pushstring(L, "BODY");
		} else {
			char buf[8];
			sprintf(buf, "BODY%d", i + 1);
			lua_pushstring(L, buf);
		}

		lua_newtable(L);
		parse_body(L, (char *)row->body[i].iov_base);
		lua_settable(L, -3);  /* BODY */
	}
	return 0;
}

static int
lbox_xlog_parser_next(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.next(xlog_pointer)");
	xlog_cursor *cur = new xlog_cursor;
	xlog_cursor_open(cur, plog);
	struct xrow_header row;
	if (next_row(L, cur, &row) == 0) {
		delete cur;
		return 1;
	}
	delete cur;
	return 0;
}

static int
iter(struct lua_State *L)
{
	xlog_cursor *cur = (xlog_cursor *)lua_touserdata(L, 1);
	int i = luaL_checkinteger(L, 2);
	struct xrow_header row;

	lua_pushinteger(L, i + 1);
	if (next_row(L, cur, &row) == 0) {
		return 2;
	}
	else {
		xlog_cursor_close(cur);
		delete cur;
		return 0;
	}

}

static int
lbox_xlog_parser_iterate(struct lua_State *L)
{

	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.iterate(xlog_pointer)");
	xlog_cursor *cur = new xlog_cursor;
	xlog_cursor_open(cur, plog);
	lua_pushcclosure(L, &iter, 1);
	lua_pushlightuserdata(L, cur);
	lua_pushinteger(L, 0);
	return 3;
}

static const struct luaL_reg xlog_parser_lib [] = {
	{"open", lbox_xlog_parser_open},
	{"close", lbox_xlog_parser_close},
	{"iterate", lbox_xlog_parser_iterate},
	{"next", lbox_xlog_parser_next},
	{NULL, NULL}
};

/** Initialize box.xlog.parser package. */
void
box_lua_xlog_parser_init(struct lua_State *L)
{

	luaL_register_module(L, "xlog.parser", xlog_parser_lib);

	lua_newtable(L);
	lua_setmetatable(L, -2);
	lua_pop(L, 1);
}


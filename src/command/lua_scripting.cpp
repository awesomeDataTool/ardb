/*
 *Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "db/db.hpp"
#include "lua_scripting.hpp"
#include "logger.hpp"
#include "util/rand.h"
#include "thread/lock_guard.hpp"
#include "thread/spin_rwlock.hpp"
#include "thread/spin_mutex_lock.hpp"
#include "thread/thread_mutex_lock.hpp"
#include "util/file_helper.hpp"
#include <string.h>
#include <limits>
#include <math.h>
#include <libgen.h>

#define MAX_LUA_STR_SIZE 1024

namespace ardb
{
    extern "C"
    {
        int (luaopen_cjson)(lua_State *L);
        int (luaopen_struct)(lua_State *L);
        int (luaopen_cmsgpack)(lua_State *L);
    }
    static const char* g_lua_file = "";
    struct LuaExecContext
    {
            int64_t lua_time_start;
            const char* lua_executing_func;
            bool lua_timeout;
            bool lua_kill;
            bool lua_abort;
            Context* exec;
            LuaExecContext() :
                    lua_time_start(0), lua_executing_func(NULL), lua_timeout(false), lua_kill(false), lua_abort(false), exec(NULL)
            {
            }
    };

    //static ThreadMutex g_lua_mutex;

    /* Take a Redis reply in the Redis protocol format and convert it into a
     * Lua type. Thanks to this function, and the introduction of not connected
     * clients, it is trivial to implement the redis() lua function.
     *
     * Basically we take the arguments, execute the Redis command in the context
     * of a non connected client, then take the generated reply and convert it
     * into a suitable Lua type. With this trick the scripting feature does not
     * need the introduction of a full Redis internals API. Basically the script
     * is like a normal client that bypasses all the slow I/O paths.
     *
     * Note: in this function we do not do any sanity check as the reply is
     * generated by Redis directly. This allows us to go faster.
     * The reply string can be altered during the parsing as it is discarded
     * after the conversion is completed.
     *
     * Errors are returned as a table with a single 'err' field set to the
     * error string.
     */

    static void redisProtocolToLuaType(lua_State *lua, RedisReply& reply)
    {
        switch (reply.type)
        {
           case REDIS_REPLY_DOUBLE:
           {
        	   lua_pushnumber(lua, (lua_Number) reply.GetDouble());
        	   break;
           }
            case REDIS_REPLY_INTEGER:
            {
                lua_pushnumber(lua, (lua_Number) reply.integer);
                break;
            }
            case REDIS_REPLY_NIL:
            {
                lua_pushboolean(lua, 0);
                break;
            }
            case REDIS_REPLY_STRING:
            {
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                break;
            }
            case REDIS_REPLY_STATUS:
            {
                lua_newtable(lua);
                lua_pushstring(lua, "ok");
                reply_status_string(reply.integer, reply.str);
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                lua_settable(lua, -3);
                break;
            }
            case REDIS_REPLY_ERROR:
            {
                lua_newtable(lua);
                lua_pushstring(lua, "err");
                reply_error_string(reply.integer, reply.str);
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                lua_settable(lua, -3);
                break;
            }
            case REDIS_REPLY_ARRAY:
            {
                lua_newtable(lua);
                for (uint32 j = 0; j < reply.MemberSize(); j++)
                {
                    lua_pushnumber(lua, j + 1);
                    redisProtocolToLuaType(lua, reply.MemberAt(j));
                    lua_settable(lua, -3);
                }
                break;
            }
            default:
            {
                abort();
            }
        }
    }

    /* Set an array of Redis String Objects as a Lua array (table) stored into a
     * global variable. */
    static void luaSetGlobalArray(lua_State *lua, const std::string& var, const StringArray& elev)
    {
        uint32 j;

        lua_newtable(lua);
        for (j = 0; j < elev.size(); j++)
        {
            lua_pushlstring(lua, elev[j].data(), elev[j].size());
            lua_rawseti(lua, -2, j + 1);
        }
        lua_setglobal(lua, var.c_str());
    }

    /* This function installs metamethods in the global table _G that prevent
     * the creation of globals accidentally.
     *
     * It should be the last to be called in the scripting engine initialization
     * sequence, because it may interact with creation of globals. */
    static void scriptingEnableGlobalsProtection(lua_State *lua)
    {
        const char *s[32];
        std::string code;
        int j = 0;

        /* strict.lua from: http://metalua.luaforge.net/src/lib/strict.lua.html.
         * Modified to be adapted to Redis. */
        s[j++] = "local mt = {}\n";
        s[j++] = "setmetatable(_G, mt)\n";
        s[j++] = "mt.__newindex = function (t, n, v)\n";
        s[j++] = "  if debug.getinfo(2) then\n";
        s[j++] = "    local w = debug.getinfo(2, \"S\").what\n";
        s[j++] = "    if w ~= \"main\" and w ~= \"C\" then\n";
        s[j++] = "      error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n";
        s[j++] = "    end\n";
        s[j++] = "  end\n";
        s[j++] = "  rawset(t, n, v)\n";
        s[j++] = "end\n";
        s[j++] = "mt.__index = function (t, n)\n";
        s[j++] = "  if debug.getinfo(2) and debug.getinfo(2, \"S\").what ~= \"C\" then\n";
        s[j++] = "    error(\"Script attempted to access unexisting global variable '\"..tostring(n)..\"'\", 2)\n";
        s[j++] = "  end\n";
        s[j++] = "  return rawget(t, n)\n";
        s[j++] = "end\n";
        s[j++] = NULL;

        for (j = 0; s[j] != NULL; j++)
        {
            code.append(s[j]);
        }
        luaL_loadbuffer(lua, code.c_str(), code.size(), "@enable_strict_lua");
        lua_pcall(lua, 0, 0, 0);
    }
    static void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc)
    {
        lua_pushcfunction(lua, luafunc);
        lua_pushstring(lua, libname);
        lua_call(lua, 1, 0);
    }

    static void luaPushError(lua_State *lua, const char *error)
    {
        lua_Debug dbg;

        lua_newtable(lua);
        lua_pushstring(lua, "err");

        /* Attempt to figure out where this function was called, if possible */
        if (lua_getstack(lua, 1, &dbg) && lua_getinfo(lua, "nSl", &dbg))
        {
            char tmp[MAX_LUA_STR_SIZE];
            snprintf(tmp, MAX_LUA_STR_SIZE - 1, "%s: %d: %s", dbg.source, dbg.currentline, error);
            lua_pushstring(lua, tmp);
        }
        else
        {
            lua_pushstring(lua, error);
        }
        lua_settable(lua, -3);
    }

    static int luaReplyToRedisReply(lua_State *lua, RedisReply& reply)
    {
        int t = lua_type(lua, -1);
        switch (t)
        {
            case LUA_TSTRING:
            {
                reply.type = REDIS_REPLY_STRING;
                reply.str.append((char*) lua_tostring(lua, -1), lua_strlen(lua, -1));
                break;
            }
            case LUA_TBOOLEAN:
                if (lua_toboolean(lua, -1))
                {
                    reply.type = REDIS_REPLY_INTEGER;
                    reply.integer = 1;
                }
                else
                {
                    reply.type = REDIS_REPLY_NIL;
                }
                break;
            case LUA_TNUMBER:
                reply.type = REDIS_REPLY_INTEGER;
                reply.integer = (long long) lua_tonumber(lua, -1);
                break;
            case LUA_TTABLE:
                /* We need to check if it is an array, an error, or a status reply.
                 * Error are returned as a single element table with 'err' field.
                 * Status replies are returned as single element table with 'ok' field */
                lua_pushstring(lua, "err");
                lua_gettable(lua, -2);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING)
                {
                    std::string err = lua_tostring(lua, -1);
                    string_replace(err, "\r\n", " ");
                    reply.type = REDIS_REPLY_ERROR;
                    reply.str = err;
                    lua_pop(lua, 2);
                    return 0;
                }

                lua_pop(lua, 1);
                lua_pushstring(lua, "ok");
                lua_gettable(lua, -2);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING)
                {
                    std::string ok = lua_tostring(lua, -1);
                    string_replace(ok, "\r\n", " ");
                    reply.str = ok;
                    reply.type = REDIS_REPLY_STATUS;
                    lua_pop(lua, 1);
                }
                else
                {
                    //void *replylen = addDeferredMultiBulkLength(c);
                    int j = 1, mbulklen = 0;

                    lua_pop(lua, 1);
                    /* Discard the 'ok' field value we popped */
                    reply.type = REDIS_REPLY_ARRAY;
                    while (1)
                    {
                        lua_pushnumber(lua, j++);
                        lua_gettable(lua, -2);
                        t = lua_type(lua, -1);
                        if (t == LUA_TNIL)
                        {
                            lua_pop(lua, 1);
                            break;
                        }
                        RedisReply& r = reply.AddMember();
                        luaReplyToRedisReply(lua, r);
                        mbulklen++;
                    }
                }
                break;
            default:
            {
                reply.type = REDIS_REPLY_NIL;
                break;
            }

        }
        lua_pop(lua, 1);
        return 0;
    }

    typedef TreeMap<std::string, std::string>::Type ScriptCache;
    typedef TreeSet<LuaExecContext*>::Type ExecContextSet;
    static SpinMutexLock g_lua_lock;
    static ScriptCache g_script_cache;
    static ExecContextSet g_script_ctxs;

    LUAInterpreter::LUAInterpreter() :
            m_lua(NULL)
    {
        Init();
    }

    static std::string* get_script_from_cache(const std::string& funcname)
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        ScriptCache::iterator found = g_script_cache.find(funcname);
        if (found == g_script_cache.end())
        {
            return NULL;
        }
        return &(found->second);
    }

    static void save_script_to_cache(const std::string& funcname, const std::string& body)
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        if (body == "")
        {
            g_script_cache.erase(funcname);
        }
        else
        {
            g_script_cache[funcname] = body;
        }
    }

    static void clear_script_cache()
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        g_script_cache.clear();
    }

    static void save_exec_ctx(LuaExecContext* ctx)
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        g_script_ctxs.insert(ctx);
    }
    static void erase_exec_ctx(LuaExecContext* ctx)
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        g_script_ctxs.erase(ctx);
    }
    static void kill_luafunc(const std::string& func)
    {
        LockGuard<SpinMutexLock> guard(g_lua_lock);
        ExecContextSet::iterator it = g_script_ctxs.begin();
        while (it != g_script_ctxs.end())
        {
            LuaExecContext* ctx = *it;
            if (!strcmp(func.c_str(), ctx->lua_executing_func) || func == "")
            {
                ctx->lua_kill = true;
            }
            it++;
        }
    }

    /* Define a lua function with the specified function name and body.
     * The function name musts be a 2 characters long string, since all the
     * functions we defined in the Lua context are in the form:
     *
     *   f_<hex sha1 sum>
     *
     * On success REDIS_OK is returned, and nothing is left on the Lua stack.
     * On error REDIS_ERR is returned and an appropriate error is set in the
     * client context. */
    int LUAInterpreter::CreateLuaFunction(const std::string& funcname, const std::string& body, std::string& err)
    {
        std::string funcdef = "function ";
        funcdef.append(funcname);
        funcdef.append("() ");
        funcdef.append(body);
        funcdef.append(" end");

        if (luaL_loadbuffer(m_lua, funcdef.c_str(), funcdef.size(), "@user_script"))
        {
            err.append("Error compiling script (new function): ").append(lua_tostring(m_lua, -1)).append("\n");
            lua_pop(m_lua, 1);
            return -1;
        }
        if (lua_pcall(m_lua, 0, 0, 0))
        {
            err.append("Error running script (new function): ").append(lua_tostring(m_lua, -1)).append("\n");
            lua_pop(m_lua, 1);
            return -1;
        }

        /* We also save a SHA1 -> Original script map in a dictionary
         * so that we can replicate / write in the AOF all the
         * EVALSHA commands as EVAL using the original script. */
        save_script_to_cache(funcname, body);
        return 0;
    }

    int LUAInterpreter::LoadLibs()
    {
        luaLoadLib(m_lua, "", luaopen_base);
        luaLoadLib(m_lua, LUA_TABLIBNAME, luaopen_table);
        luaLoadLib(m_lua, LUA_STRLIBNAME, luaopen_string);
        luaLoadLib(m_lua, LUA_MATHLIBNAME, luaopen_math);
        luaLoadLib(m_lua, LUA_DBLIBNAME, luaopen_debug);

        luaLoadLib(m_lua, "cjson", luaopen_cjson);
        luaLoadLib(m_lua, "struct", luaopen_struct);
        luaLoadLib(m_lua, "cmsgpack", luaopen_cmsgpack);
        return 0;
    }

    int LUAInterpreter::RemoveUnsupportedFunctions()
    {
        lua_pushnil(m_lua);
        lua_setglobal(m_lua, "loadfile");
        return 0;
    }

    static ThreadLocal<Context*> g_local_ctx;
    static ThreadLocal<LuaExecContext*> g_lua_exec_ctx;

    struct LuaExecContextGuard
    {
            LuaExecContext ctx;
            LuaExecContextGuard()
            {
                g_lua_exec_ctx.SetValue(&ctx);
            }
            ~LuaExecContextGuard()
            {
                g_lua_exec_ctx.SetValue(NULL);
            }
    };

    int LUAInterpreter::CallArdb(lua_State *lua, bool raise_error)
    {
        int j, argc = lua_gettop(lua);
        ArgumentArray cmdargs;

        /* Require at least one argument */
        if (argc == 0)
        {
            luaPushError(lua, "Please specify at least one argument for redis.call()");
            return 1;
        }

        /* Build the arguments vector */
        for (j = 0; j < argc; j++)
        {
            if (!lua_isstring(lua, j + 1))
                break;
            std::string arg;
            arg.append(lua_tostring(lua, j + 1), lua_strlen(lua, j + 1));
            cmdargs.push_back(arg);
        }

        /* Check if one of the arguments passed by the Lua script
         * is not a string or an integer (lua_isstring() return true for
         * integers as well). */
        if (j != argc)
        {
            luaPushError(lua, "Lua redis() command arguments must be strings or integers");
            return 1;
        }

        /* Setup our fake client for command execution */

        RedisCommandFrame cmd(cmdargs);
        Ardb::RedisCommandHandlerSetting* setting = g_db->FindRedisCommandHandlerSetting(cmd);
        /* Command lookup */
        if (NULL == setting)
        {
            luaPushError(lua, "Unknown Redis command called from Lua script");
            return -1;
        }

        /* There are commands that are not allowed inside scripts. */
        if (setting->flags & ARDB_CMD_NOSCRIPT)
        {
            luaPushError(lua, "This Redis command is not allowed from scripts");
            return -1;
        }

        //TODO consider forbid readonly slave to exec write cmd
        LuaExecContext* ctx = g_lua_exec_ctx.GetValue();
        Context* lua_ctx = ctx->exec;
        RedisReply& reply = lua_ctx->GetReply();
        reply.Clear();
        g_db->DoCall(*lua_ctx, *setting, cmd);
        if (raise_error && reply.type != REDIS_REPLY_ERROR)
        {
            raise_error = 0;
        }
        redisProtocolToLuaType(lua, reply);

        if (raise_error)
        {
            /* If we are here we should have an error in the stack, in the
             * form of a table with an "err" field. Extract the string to
             * return the plain error. */
            lua_pushstring(lua, "err");
            lua_gettable(lua, -2);
            return lua_error(lua);
        }
        return 1;
    }

    int LUAInterpreter::PCall(lua_State *lua)
    {
        return CallArdb(lua, true);
    }

    int LUAInterpreter::Call(lua_State *lua)
    {
        return CallArdb(lua, false);
    }

    static void print_lua_table(lua_State *L, int index, std::string& str)
    {
        // Push another reference to the table on top of the stack (so we know
        // where it is, and this function can work for negative, positive and
        // pseudo indices
        str.append("{");
        lua_pushvalue(L, index);
        // stack now contains: -1 => table
        lua_pushnil(L);
        // stack now contains: -1 => nil; -2 => table
        int count = 0;
        while (lua_next(L, -2))
        {
            if (count > 0)
            {
                str.append(";");
            }
            // stack now contains: -1 => value; -2 => key; -3 => table
            // copy the key so that lua_tostring does not modify the original
            lua_pushvalue(L, -2);
            // stack now contains: -1 => key; -2 => value; -3 => key; -4 => table
            const char *key = lua_tostring(L, -1);
            const char *value = lua_tostring(L, -2);
            char tmp[1024];
            sprintf(tmp, "%s => %s", key, value);
            str.append(tmp);
            //printf("%s => %s\n", key, value);
            // pop value + copy of key, leaving original key
            lua_pop(L, 2);
            count++;
            // stack now contains: -1 => key; -2 => table
        }
        // stack now contains: -1 => table (when lua_next returns 0 it pops the key
        // but does not push anything.)
        // Pop table
        lua_pop(L, 1);
        // Stack is now the same as it was on entry to this function
        str.append("}");
    }

    int LUAInterpreter::Assert2(lua_State *lua)
    {
        lua_Debug ar;
        lua_getstack(lua, 1, &ar);
        lua_getinfo(lua, "nSl", &ar);
        int lua_line = ar.currentline;

        int argc = lua_gettop(lua);

        /* Require at least one argument */
        if (argc != 2)
        {
            luaPushError(lua, "Please specify 2 arguments for assert2()");
            return -1;
        }
        if (!lua_isboolean(lua, 1))
        {
            luaPushError(lua, "Lua assert2() command argument[0] must be boolean");
            return -1;
        }

        int b = lua_toboolean(lua, 1);
        if (b)
        {
            fprintf(stdout, "\e[1;32m%-6s\e[m %s:%d\n", "[PASS]", g_lua_file, lua_line);
        }
        else
        {
            LuaExecContext* ctx = g_lua_exec_ctx.GetValue();
            ctx->lua_abort = true;
            const char* actual = NULL;
            std::string tablestr;
            int obj_type = lua_type(lua, 2);
            if (lua_istable(lua, 2))
            {
                print_lua_table(lua, 2, tablestr);
                actual = tablestr.c_str();
            }
            else if (lua_isboolean(lua, 2))
            {
                actual = lua_toboolean(lua, 2) ?"true":"false";
            }
            else
            {
                actual = lua_tolstring(lua, 2, NULL);
            }
            fprintf(stderr, "\e[1;35m%-6s\e[m %s:%d Actual value is %d:%s\n", "[FAIL]", g_lua_file, lua_line, obj_type, actual);
            lua_pushstring(lua, "Assert2 failed...");
            lua_error(lua);
            return -1;
        }
        return 1;
    }

    int LUAInterpreter::Log(lua_State *lua)
    {
        int j, argc = lua_gettop(lua);
        int level;
        std::string log;
        if (argc < 2)
        {
            luaPushError(lua, "redis.log() requires two arguments or more.");
            return 1;
        }
        else if (!lua_isnumber(lua, -argc))
        {
            luaPushError(lua, "First argument must be a number (log level).");
            return 1;
        }
        level = (int) lua_tonumber(lua, -argc);
        if (level < FATAL_LOG_LEVEL || level > TRACE_LOG_LEVEL)
        {
            luaPushError(lua, "Invalid debug level.");
            return 1;
        }

        /* Glue together all the arguments */
        for (j = 1; j < argc; j++)
        {
            size_t len;
            char *s;

            s = (char*) lua_tolstring(lua, (-argc) + j, &len);
            if (s)
            {
                if (j != 1)
                {
                    log.append(" ");
                }
                log.append(s, len);
            }
        }
        LOG_WITH_LEVEL((LogLevel )level, "%s", log.c_str());
        return 0;
    }

    int LUAInterpreter::SHA1Hex(lua_State *lua)
    {
        int argc = lua_gettop(lua);
        size_t len;
        char *s;

        if (argc != 1)
        {
            luaPushError(lua, "wrong number of arguments");
            return 1;
        }

        s = (char*) lua_tolstring(lua, 1, &len);
        std::string digest = sha1_sum_data(s, len);
        lua_pushstring(lua, digest.c_str());
        return 1;
    }

    int LUAInterpreter::ReturnSingleFieldTable(lua_State *lua, const std::string& field)
    {
        if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING)
        {
            luaPushError(lua, "wrong number or type of arguments");
            return 1;
        }

        lua_newtable(lua);
        lua_pushstring(lua, field.c_str());
        lua_pushvalue(lua, -3);
        lua_settable(lua, -3);
        return 1;
    }

    int LUAInterpreter::ErrorReplyCommand(lua_State *lua)
    {
        return ReturnSingleFieldTable(lua, "err");
    }
    int LUAInterpreter::StatusReplyCommand(lua_State *lua)
    {
        return ReturnSingleFieldTable(lua, "ok");
    }
    int LUAInterpreter::MathRandom(lua_State *L)
    {
        /* the `%' avoids the (rare) case of r==1, and is needed also because on
         some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
        lua_Number r = (lua_Number) (redisLrand48() % REDIS_LRAND48_MAX) / (lua_Number) REDIS_LRAND48_MAX;
        switch (lua_gettop(L))
        { /* check number of arguments */
            case 0:
            { /* no arguments */
                lua_pushnumber(L, r); /* Number between 0 and 1 */
                break;
            }
            case 1:
            { /* only upper limit */
                int u = luaL_checkint(L, 1);
                luaL_argcheck(L, 1 <= u, 1, "interval is empty");
                lua_pushnumber(L, floor(r * u) + 1); /* int between 1 and `u' */
                break;
            }
            case 2:
            { /* lower and upper limits */
                int l = luaL_checkint(L, 1);
                int u = luaL_checkint(L, 2);
                luaL_argcheck(L, l <= u, 2, "interval is empty");
                lua_pushnumber(L, floor(r * (u - l + 1)) + l); /* int between `l' and `u' */
                break;
            }
            default:
                return luaL_error(L, "wrong number of arguments");
        }
        return 1;
    }
    int LUAInterpreter::MathRandomSeed(lua_State *lua)
    {
        redisSrand48(luaL_checkint(lua, 1));
        return 0;
    }

    void LUAInterpreter::MaskCountHook(lua_State *lua, lua_Debug *ar)
    {
        ARDB_NOTUSED(ar);
        ARDB_NOTUSED(lua);
        LuaExecContext* ctx = g_lua_exec_ctx.GetValue();
        uint64 elapsed = get_current_epoch_millis() - ctx->lua_time_start;
        if (elapsed >= (uint64) g_config->lua_time_limit && !ctx->lua_timeout)
        {
            WARN_LOG("Lua slow script detected: %s still in execution after %llu milliseconds. You can try killing the script using the SCRIPT KILL command.",
                    ctx->lua_executing_func, elapsed);
            ctx->lua_timeout = true;
            if (NULL != ctx->exec->client)
            {
                Channel* client = ctx->exec->client->client;
                client->DetachFD();
            }
        }
        if (ctx->lua_timeout && NULL != ctx->exec->client)
        {
            Channel* client = ctx->exec->client->client;
            client->GetService().Continue();
        }
        if (ctx->lua_kill)
        {
            WARN_LOG("Lua script killed by user with SCRIPT KILL.");
            lua_pushstring(lua, "Script killed by user with SCRIPT KILL...");
            lua_error(lua);
        }
        else if (ctx->lua_abort)
        {
            WARN_LOG("Lua script:%s abort by assert2.", g_lua_file);
            lua_pushstring(lua, "Script killed by user with SCRIPT KILL...");
            lua_error(lua);
        }
    }

    int LUAInterpreter::Init()
    {
        m_lua = lua_open();

        LoadLibs();
        RemoveUnsupportedFunctions();

        /* Register the redis commands table and fields */
        lua_newtable(m_lua);

        /* redis.call */
        lua_pushstring(m_lua, "call");
        lua_pushcfunction(m_lua, LUAInterpreter::Call);
        lua_settable(m_lua, -3);

        /* redis.pcall */
        lua_pushstring(m_lua, "pcall");
        lua_pushcfunction(m_lua, LUAInterpreter::PCall);
        lua_settable(m_lua, -3);

        /* redis.assert2 */
        lua_pushstring(m_lua, "assert2");
        lua_pushcfunction(m_lua, LUAInterpreter::Assert2);
        lua_settable(m_lua, -3);

        /* redis.log and log levels. */
        lua_pushstring(m_lua, "log");
        lua_pushcfunction(m_lua, LUAInterpreter::Log);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_DEBUG");
        lua_pushnumber(m_lua, DEBUG_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_VERBOSE");
        lua_pushnumber(m_lua, TRACE_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_NOTICE");
        lua_pushnumber(m_lua, INFO_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_WARNING");
        lua_pushnumber(m_lua, WARN_LOG_LEVEL);
        lua_settable(m_lua, -3);

        /* redis.sha1hex */
        lua_pushstring(m_lua, "sha1hex");
        lua_pushcfunction(m_lua, LUAInterpreter::SHA1Hex);
        lua_settable(m_lua, -3);

        /* redis.error_reply and redis.status_reply */
        lua_pushstring(m_lua, "error_reply");
        lua_pushcfunction(m_lua, LUAInterpreter::ErrorReplyCommand);
        lua_settable(m_lua, -3);
        lua_pushstring(m_lua, "status_reply");
        lua_pushcfunction(m_lua, LUAInterpreter::StatusReplyCommand);
        lua_settable(m_lua, -3);

        /* Finally set the table as 'redis' global var. */
        lua_setglobal(m_lua, "redis");
        lua_getglobal(m_lua, "redis");
        lua_setglobal(m_lua, "ardb");

        /* Replace math.random and math.randomseed with our implementations. */
        lua_getglobal(m_lua, LUA_MATHLIBNAME);
        if (lua_isnil(m_lua, -1))
        {
            ERROR_LOG("Failed to load lib math");
        }
        lua_pushstring(m_lua, "random");
        lua_pushcfunction(m_lua, LUAInterpreter::MathRandom);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "randomseed");
        lua_pushcfunction(m_lua, LUAInterpreter::MathRandomSeed);
        lua_settable(m_lua, -3);

        lua_setglobal(m_lua, "math");

        /* Add a helper function we use for pcall error reporting.
         * Note that when the error is in the C function we want to report the
         * information about the caller, that's what makes sense from the point
         * of view of the user debugging a script. */
        {
            const char *errh_func = "function __redis__err__handler(err)\n"
                    "  local i = debug.getinfo(2,'nSl')\n"
                    "  if i and i.what == 'C' then\n"
                    "    i = debug.getinfo(3,'nSl')\n"
                    "  end\n"
                    "  if i then\n"
                    "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                    "  else\n"
                    "    return err\n"
                    "  end\n"
                    "end\n";
            luaL_loadbuffer(m_lua, errh_func, strlen(errh_func), "@err_handler_def");
            lua_pcall(m_lua, 0, 0, 0);
        }
        scriptingEnableGlobalsProtection(m_lua);
        return 0;
    }

    int LUAInterpreter::Eval(Context& ctx, const std::string& func, const StringArray& keys, const StringArray& args, bool isSHA1Func)
    {
        RedisReply& reply = ctx.GetReply();
        //DEBUG_LOG("Exec script:%s", func.c_str());
        //g_local_ctx.SetValue(&ctx);
        LuaExecContextGuard guard;
        redisSrand48(0);
        std::string err;
        std::string funcname = "f_";
        const std::string* funptr = &func;
        if (isSHA1Func)
        {
            if (func.size() != 40)
            {
                reply.SetErrCode(ERR_NOSCRIPT);
                return 0;
            }
            funcname.append(func);
        }
        else
        {
            funcname.append(sha1_sum(func));
        }
        /* Push the pcall error handler function on the stack. */
        lua_getglobal(m_lua, "__redis__err__handler");

        lua_getglobal(m_lua, funcname.c_str());
        if (lua_isnil(m_lua, -1))
        {
            lua_pop(m_lua, 1);
            /* remove the nil from the stack */
            /* Function not defined... let's define it if we have the
             * body of the function. If this is an EVALSHA call we can just
             * return an error. */
            if (isSHA1Func)
            {
                funptr = get_script_from_cache(funcname);
                if (NULL == funptr)
                {
                    lua_pop(m_lua, 1);
                    /* remove the error handler from the stack. */
                    reply.SetErrCode(ERR_NOSCRIPT);
                    return 0;
                }
            }
            else
            {
                save_script_to_cache(funcname, func);
            }
            if (CreateLuaFunction(funcname, *funptr, err))
            {
                reply.SetErrorReason(err);
                lua_pop(m_lua, 1);
                return -1;
            }
            lua_getglobal(m_lua, funcname.c_str());
        }

        /* Populate the argv and keys table accordingly to the arguments that
         * EVAL received. */
        luaSetGlobalArray(m_lua, "KEYS", keys);
        luaSetGlobalArray(m_lua, "ARGV", args);

        bool delhook = false;
        if (g_config->lua_time_limit > 0)
        {
            lua_sethook(m_lua, MaskCountHook, LUA_MASKCOUNT, 100000);
            delhook = true;
        }
        guard.ctx.exec = &ctx;
        guard.ctx.lua_time_start = get_current_epoch_millis();
        guard.ctx.lua_executing_func = funcname.c_str() + 2;
        guard.ctx.lua_kill = false;
        save_exec_ctx(&guard.ctx);

//        LockGuard<ThreadMutex> guard(g_lua_mutex, g_db->GetConfig().lua_exec_atomic); //only one transc allowed exec at the same time in multi threads
        int errid = lua_pcall(m_lua, 0, 1, -2);
        erase_exec_ctx(&guard.ctx);
        if (delhook)
        {
            lua_sethook(m_lua, MaskCountHook, 0, 0); /* Disable hook */
        }
        /* Call the Lua garbage collector from time to time to avoid a
         * full cycle performed by Lua, which adds too latency.
         *
         * The call is performed every LUA_GC_CYCLE_PERIOD executed commands
         * (and for LUA_GC_CYCLE_PERIOD collection steps) because calling it
         * for every command uses too much CPU. */
#define LUA_GC_CYCLE_PERIOD 50
        {
            static long gc_count = 0;

            gc_count++;
            if (gc_count == LUA_GC_CYCLE_PERIOD)
            {
                lua_gc(m_lua, LUA_GCSTEP, LUA_GC_CYCLE_PERIOD);
                gc_count = 0;
            }
        }
        if (guard.ctx.lua_timeout)
        {
            if ( NULL != ctx.client)
            {
                Channel* client = ctx.client->client;
                client->AttachFD();
            }
        }
        if (errid)
        {
            char tmp[1024];
            snprintf(tmp, 1023, "Error running script (call to %s): %s\n", funcname.c_str(), lua_tostring(m_lua, -1));
            reply.SetErrorReason(tmp);
            lua_pop(m_lua, 2);
            /*  Consume the Lua reply and remove error handler. */
        }
        else
        {
            /* On success convert the Lua return value into Redis reply */
            reply.Clear();
            luaReplyToRedisReply(m_lua, reply);
            lua_pop(m_lua, 1); /* Remove the error handler. */
        }
        return 0;
    }

    int LUAInterpreter::Load(const std::string& func, std::string& ret)
    {
        std::string funcname = "f_";
        ret.clear();
        ret = sha1_sum(func);
        funcname.append(ret);
        return CreateLuaFunction(funcname, func, ret) == 0;
    }

    int LUAInterpreter::EvalFile(Context& ctx, const std::string& file)
    {
        std::string content;
        file_read_full(file, content);
        StringArray keys, args;
        char *path2 = strdup(file.c_str());
        g_lua_file = basename(path2);
        int ret = Eval(ctx, content, keys, args, false);
        g_lua_file = "";
        free(path2);
        return ret;
    }

    void LUAInterpreter::Reset()
    {
        lua_close(m_lua);
        Init();
    }

    LUAInterpreter::~LUAInterpreter()
    {
        lua_close(m_lua);
    }

    int Ardb::Eval(Context& ctx, RedisCommandFrame& cmd)
    {
        uint32 numkey = 0;
        RedisReply& reply = ctx.GetReply();
        if (!string_touint32(cmd.GetArguments()[1], numkey))
        {
            reply.SetErrCode(ERR_INVALID_INTEGER_ARGS);
            //fill_error_reply(ctx.reply, "value is not an integer or out of range");
            return 0;
        }
        if (cmd.GetArguments().size() < numkey + 2)
        {
            reply.SetErrCode(ERR_INVALID_SYNTAX);
            //fill_error_reply(ctx.reply, "Wrong number of arguments for Eval");
            return 0;
        }
        LUAInterpreter& interpreter = m_lua.GetValue();
        StringArray keys, args;
        for (uint32 i = 2; i < numkey + 2; i++)
        {
            keys.push_back(cmd.GetArguments()[i]);
        }
        for (uint32 i = numkey + 2; i < cmd.GetArguments().size(); i++)
        {
            args.push_back(cmd.GetArguments()[i]);
        }
        interpreter.Eval(ctx, cmd.GetArguments()[0], keys, args, cmd.GetType() == REDIS_CMD_EVALSHA);
        return 0;
    }

    int Ardb::EvalSHA(Context& ctx, RedisCommandFrame& cmd)
    {
        return Eval(ctx, cmd);
    }

    int Ardb::Script(Context& ctx, RedisCommandFrame& cmd)
    {
        RedisReply& reply = ctx.GetReply();
        const std::string& subcommand = cmd.GetArguments()[0];
        if (!strcasecmp(subcommand.c_str(), "EXISTS"))
        {
            reply.ReserveMember(0);
            for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
            {
                RedisReply& r = reply.AddMember();
                std::string funcname = "f_";
                funcname.append(cmd.GetArguments()[i]);
                r.SetInteger(get_script_from_cache(funcname) != NULL ? 1 : 0);
            }
            return 0;
        }
        else if (!strcasecmp(subcommand.c_str(), "FLUSH"))
        {
            if (cmd.GetArguments().size() != 1)
            {
                reply.SetErrCode(ERR_INVALID_ARGS);
                //fill_error_reply(ctx.reply, "wrong number of arguments for SCRIPT FLUSH");
            }
            else
            {
                clear_script_cache();
                reply.SetStatusCode(STATUS_OK);
            }
        }
        /*
         * NOTE: 'SCRIPT KILL' may need func's sha1 as argument because ardb may run in multithreading mode,
         *       while more than ONE scripts may be running at the same time.
         *       Redis do NOT need the argument.
         */
        else if (!strcasecmp(subcommand.c_str(), "KILL"))
        {
            if (cmd.GetArguments().size() > 2)
            {
                //fill_error_reply(ctx.reply, "wrong number of arguments for SCRIPT KILL");
                reply.SetErrCode(ERR_INVALID_ARGS);
            }
            else
            {
                if (cmd.GetArguments().size() == 2)
                {
                    kill_luafunc(cmd.GetArguments()[1]);
                }
                else
                {
                    kill_luafunc("");
                }
                reply.SetStatusCode(STATUS_OK);
            }
        }
        else if (!strcasecmp(subcommand.c_str(), "LOAD"))
        {
            if (cmd.GetArguments().size() != 2)
            {
                reply.SetErrCode(ERR_INVALID_ARGS);
                //fill_error_reply(ctx.reply, "wrong number of arguments for SCRIPT LOAD");
            }
            else
            {
                std::string result;
                if (m_lua.GetValue().Load(cmd.GetArguments()[1], result))
                {
                    reply.SetString(result);
                }
                else
                {
                    reply.SetErrorReason(result);
                }
            }
        }
        else
        {
            reply.SetErrCode(ERR_INVALID_SYNTAX);
            //fill_error_reply(ctx.reply, "Syntax error, try SCRIPT (EXISTS | FLUSH | KILL | LOAD)");
        }
        return 0;
    }
}


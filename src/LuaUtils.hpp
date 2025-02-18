/* =====================================================================================
 * Utilitiy functions, macros and templates for binding C++ code to Lua easily
 *
 * Copyright (C) 2018 Jonas Møller (no) <jonasmo441@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * =====================================================================================
 */

/** @file LuaUtils.hpp
 *
 * @brief Lua binding utilities.
 */

#pragma once

#include <functional> 
#include <unordered_map>
#include <atomic>
#include <iostream>
#include <sstream>
#include <memory>
#include <cstring>

extern "C" {
    #include <lua.h>
    #include <lauxlib.h>
    #include <lualib.h>
    #include <libgen.h>
}

/** FIXME: This library is NOT THREAD SAFE!!!
 *
 * The problem lies with LuaMethod.setState().
 * You have to change the API so that the function
 * returned by LuaMethod.bind() takes the lua_State
 * as an argument.
 *
 * The LuaMethod instance has been set to local, this
 * might work just fine.
 */

/** How to bind classes to Lua using this API:
 *
 * From here on out, when `Module` is written, you should replace it with
 * the name of the class you want to export.
 *
 * At the top of your header file, declare which methods you want to export:
 *
 *     #define Module_lua_methods(M, _) \
 *             M(Module, method_name_01, int(), int(), int()) \
 *             M(Module, method_name_02, float(), std::string())
 *
 * Then, to define the wrapper functions (prototypes):
 *
 *     // Declares prototypes for the functions that will be given to Lua
 *     // later on, these have C linkage.
 *     LUA_DECLARE(Module_lua_methods)
 *
 * Add the following to your class:
 *
 *     class Module : LuaIface<Module> ... {
 *         private:
 *             ...
 *             // Defines `Module::Module_lua_methods` to be a `luaL_Reg`, which
 *             // binds method names to the prototypes defined with `LUA_DECLARE`
 *             LUA_METHOD_COLLECT(Module_lua_methods);
 *         public:
 *             ...
 *             // Extracts methods from your class, so that they can be called like
 *             // this: `Module::extracted_method(module_instance, ...)`
 *             LUA_EXTRACT(Module_lua_methods)
 *     }
 *
 * Then in your implementation file, change your Module::Module initializer
 * so that it intializes LuaIface:
 * 
 *     Module(...) : LuaIface(this, Module_lua_methods) ... {
 *         ...
 *     }
 *
 * And then finally at the bottom of your implementation file:
 * 
 *     LUA_CREATE_BINDINGS(Module_lua_methods)
 *
 * Which will actually create implementations of the functions
 * defined with `LUA_DECLARE`.
 *
 * In order to use this interface for your class, do the following:
 *
 *     // Open up a new Lua state 
 *     lua_State *L = luaL_newstate();
 *     Module *instance = new Module();
 *     // Open the Lua library inside the state with the name "Module"
 *     instance->luaOpen(L, "Module");
 *
 *     // Your class can now be used inside of Lua like this:
 *     luaL_dostring(L, "Module:method_name_01(1, 2, 3)")
 *
 * Runtime argument checking is performed when these functions
 * are called from Lua, errors are in the following format:
 *
 *     Error: expected (userdata, number, number, number) got (userdata, boolean, string, number)
 *
 * Where `userdata` refers to the `Module` instance.
 */

#define LUA_METHOD_NAME(_T, _m) __bind_##_T##_##_m
#define LUA_METHOD_EXTRACT_NAME(_m) __extracted_method_##_m

#define LUA_METHOD_EXTRACT(_T, _method, ...) \
        template <class... Atoms> \
        static inline auto LUA_METHOD_EXTRACT_NAME(_method)(Atoms... args) noexcept { \
            return [](_T *self, Atoms... nargs) -> decltype(((_T *) nullptr)->_method(args...)) { \
                return self->_method(nargs...); \
            }; \
        }

#define LUA_METHOD_DECLARE(_T, _m, ...) \
        int LUA_METHOD_NAME(_T, _m)(lua_State *L)

#define LUA_METHOD_BIND(_T, _m, ...) \
        int LUA_METHOD_NAME(_T, _m)(lua_State *L) { \
            thread_local static const auto ex_fn = _T::LUA_METHOD_EXTRACT_NAME(_m)(__VA_ARGS__); \
            thread_local static Lua::LuaMethod<_T> bind; \
            thread_local static const auto fn = bind.wrap(#_T"::"#_m, ex_fn, (_T *)nullptr, ##__VA_ARGS__); \
            bind.setState(L); \
            return fn(); \
        }

#define LUA_DECLARE(_Mmap) \
        extern "C" { \
            _Mmap(LUA_METHOD_DECLARE, ;); \
        }
#define LUA_EXTRACT(_Mmap) _Mmap(LUA_METHOD_EXTRACT, )
#define LUA_CREATE_BINDINGS(_Mmap) \
        extern "C" { \
            _Mmap(LUA_METHOD_BIND, ) \
        }

#define _lua_utils_comma ,

#define LUA_METHOD_COLLECT_SINGLE(_T, _m, ...) {#_m , LUA_METHOD_NAME(_T, _m)}

#define LUA_METHOD_COLLECT(_Mmap) \
        static constexpr luaL_Reg _Mmap[] = { \
            _Mmap(LUA_METHOD_COLLECT_SINGLE, _lua_utils_comma), \
            {NULL, NULL} \
        }


namespace Lua {
    class LuaError : public std::exception {
    private:
        std::string expl;

    public:
        std::vector<lua_Debug> trace;

        explicit LuaError(const std::string& expl,
                          const std::vector<lua_Debug>& trace)
            : expl(expl),
              trace(trace)
        {
            fmtError();
            std::cout << "trace size: " << this->trace.size() << std::endl;
        }

        explicit LuaError(const std::string& expl)
            : expl(expl),
              trace{}
        {
            fmtError();
        }

        /** Lua errors are reported like this:
         *  /long/winding/path/file.lua:<line>: <error message>
         *  We are only interested in this part:
         *  file.lua:<line>: <error message>
         *  
         * If your paths have the ':' symbol in them this function will
         * break, but so will many other things, like environment variables
         * that expect ':' to be a safe separator. The script may contain
         * one or more ':' characters without causing any problems.
         */ 
        inline const char *fmtError() const noexcept {
            const char *ptr = expl.c_str();
            const char *start = ptr;
            for (size_t i = 0; i < expl.size(); i++)
                if (ptr[i] == '/')
                    start = &ptr[i+1];
                else if (ptr[i] == ':')
                    break;
            return start;
        }

        virtual const char *what() const noexcept {
            return fmtError();
        }

        /** Format the Lua error traceback. */
        std::string fmtTraceback() const noexcept {
            std::stringstream err_ss;
            int lv = 0;
            bool in_c;
            for (const auto& ar : trace) {
                if (!strcmp(ar.what, "C")) {
                    in_c = true;
                    continue;
                } else if (in_c) {
                    err_ss << "  [.]: ... C++ ...";
                    err_ss << std::endl;
                    in_c = false;
                }
                err_ss << "  [" << lv++ << "]:";
                err_ss << " func '" << ((ar.name) ? ar.name : "unknown") << "'";
                err_ss << " @ " << basename((char*)ar.short_src);
                err_ss << ":" << ar.linedefined;
                err_ss << std::endl;
            }
            return err_ss.str();
        }

        std::string fmtReport() const noexcept {
            std::stringstream err_ss;
            err_ss << fmtError() << std::endl;
            err_ss << fmtTraceback();
            return err_ss.str();
        }
    };

    template <class T> struct LuaValue {
        void get(lua_State *, int) {}
    };

    /** Lua integers. */
    template <> struct LuaValue<int> {
        /** Retrieve an integer from the Lua state.
         *
         * @param L Lua state.
         * @param idx The index of the integer on the stack.
         */
        int get(lua_State *L, int idx) {
            if (!lua_isnumber(L, idx))
                throw LuaError("Expected number");
            return lua_tointeger(L, idx);
        }
    };

    /** Lua numbers. */
    template <> struct LuaValue<double> {
        /** Retrieve a number from the Lua state.
         *
         * @param L Lua state.
         * @param idx The index of the number on the stack.
         */
        int get(lua_State *L, int idx) {
            if (!lua_isnumber(L, idx))
                throw LuaError("Expected number");
            return lua_tonumber(L, idx);
        }
    };

    /** Lua strings */
    template <> struct LuaValue<std::string> {
        /** Retrieve a string from the Lua state.
         *
         * @param L Lua state.
         * @param idx The index of the string on the stack.
         */
        std::string get(lua_State *L, int idx) {
            size_t sz;
            const char *s = lua_tolstring(L, idx, &sz);
            return std::string(s, sz);
        }
    };

    /** Lua booleans */
    template <> struct LuaValue<bool> {
        /** Retrieve a boolean from the Lua state.
         *
         * @param L Lua state.
         * @param idx The index of the boolean on the stack.
         */
        bool get(lua_State *L, int idx) {
            if (!lua_isboolean(L, idx))
                throw LuaError("Expected boolean");
            return lua_toboolean(L, idx);
        }
    };

    template <> struct LuaValue<void> {
        void get(lua_State*, int) { }
    };

    /** Lua arrays */
    template <class T> struct LuaValue<std::vector<T>> {
        /** Retrieve an array from the Lua state.
         *
         * @param L Lua state.
         * @param idx The index of the array on the stack.
         */
        std::vector<T> get(lua_State *L, int idx) {
            std::vector<T> vec;
            lua_pushvalue(L, idx);
            if (!lua_istable(L, -1))
                throw LuaError("Expected a table");
            for (int i = 1;; i++) {
                lua_pushinteger(L, i);
                lua_gettable(L, -2);
                if (lua_isnil(L, -1))
                    break;
                vec.push_back(LuaValue<T>().get(L, -1));
                lua_pop(L, 1);
            }
            std::cout << "Got vector: " << std::endl;
            for (const auto& val : vec) {
                std::cout << "  - " << val << std::endl;
            }
            return vec;
        }
    };

    extern "C" int hwk_lua_error_handler_callback(lua_State *L) noexcept;


    static const std::string lua_type_names[LUA_NUMTAGS] = {
        "nil",           // Lua type: nil
        "boolean",       // Lua type: boolean
        "lightuserdata", // Lua type: lightuserdata
        "number",        // Lua type: number
        "string",        // Lua type: string
        "table",         // Lua type: table
        "function",      // Lua type: function
        "userdata",      // Lua type: userdata
        "thread",        // Lua type: thread
    };

    inline int luaPush(lua_State *L, bool v) noexcept {
        lua_pushboolean(L, v);
        return 1;
    }

    inline int luaPush(lua_State *L, int v) noexcept {
        lua_pushnumber(L, v);
        return 1;
    }

    inline int luaPush(lua_State *L, std::string& v) noexcept {
        lua_pushlstring(L, v.c_str(), v.length());
        return 1;
    }

    inline int luaPush(lua_State *L, const char *c) noexcept {
        lua_pushstring(L, c);
        return 1;
    }

    template <class Any>
    inline int luaPush(lua_State *, Any) noexcept {
        return 0;
    }

    inline bool luaGet(lua_State *L, bool, int idx) noexcept {
        return lua_toboolean(L, idx);
    }

    inline int luaGet(lua_State *L, int, int idx) noexcept {
        return lua_tonumber(L, idx);
    }

    inline std::string luaGet(lua_State *L, std::string, int idx) noexcept {
        return std::string(lua_tostring(L, idx));
    }

    /** Tools for binding a C++ method to Lua. */
    template <class T>
    class LuaMethod {
    private:
        lua_State *L = nullptr;

    public:
        inline void setState(lua_State *L) {
            this->L = L;
        }

        inline T *get() noexcept {
            return (T *) lua_touserdata(L, lua_upvalueindex(1));
        }

        template <class P>
        inline bool checkLuaType(int idx, P *) noexcept {
            // TODO: Add check for type of userdata
            return lua_type(L, idx) == LUA_TUSERDATA;// || lua_type(L, idx) == LUA_TLIGHTUSERDATA;
        }

        inline bool checkLuaType(int idx, int) noexcept {
            return lua_type(L, idx) == LUA_TNUMBER;
        }

        inline bool checkLuaType(int idx, std::string&&) noexcept {
            return lua_type(L, idx) == LUA_TSTRING;
        }

        inline bool checkLuaType(int idx, bool) noexcept {
            return lua_type(L, idx) == LUA_TBOOLEAN;
        }

        inline int luaGetVal(int idx, int) noexcept {
            return lua_tonumber(L, idx);
        }

        inline bool luaGetVal(int idx, bool) noexcept {
            return lua_toboolean(L, idx);
        }

        template <class P>
        inline P* luaGetVal(int idx, P *) noexcept {
            P **ptr = (P **) lua_touserdata(L, idx);
            if (ptr == nullptr) {
                fprintf(stderr, "Received null userdata pointer\n");
                abort();
            }
            return *ptr;
        }

        inline const char *luaGetVal(int idx, const char *) noexcept {
            return lua_tostring(L, idx);
        }

        static inline constexpr int varargLength() noexcept {
            return 0;
        }

        template <class Tp, class... Arg>
        static inline constexpr int varargLength(Tp&&, Arg&&... tail) noexcept {
            return 1 + varargLength(tail...);
        }

        inline bool checkArgs(int) noexcept {
            return true;
        }

        template <class Head, class... Atoms>
        inline bool checkArgs(int idx, Head&& head, Atoms&&... tail) noexcept {
            return checkLuaType(idx, head) and checkArgs(idx+1, tail...);
        }

        /** Finish off recursion in callFromLuaFunction, in this case there
         * are no arguments left to get from the Lua stack, so we just return
         * a function that will finally push the return value onto the stack
         * and return the amount of results that were pushed.
         */
        template <class R, class Fn>
        inline std::function<int()> callFromLuaFunction(int, Fn f) noexcept {
            return [f, this]() -> int {
                if constexpr (std::is_void<R>::value) {
                    (void) (this);
                    f();
                    return 0;
                } else
                    return luaPush(L, f());
            };
        }

        /** Recurse downwards creating lambda functions along the way that
         *  get values from the Lua stack with the appropriate lua_get* function.
         */
        template <class R, class Fn, class Head, class... Atoms>
        const std::function<int()> callFromLuaFunction(int idx, Fn f, Head head, Atoms... tail) noexcept {
            auto nf = [this, idx, head, f](Atoms... args) -> R {
                return f(luaGetVal(idx, head), args...);
            };
            return callFromLuaFunction<R>(idx + 1, nf, tail...);
        }

        /* switch(Type) { */
        const std::string typeString(std::string  ) noexcept { return "string";  }
        const std::string typeString(const char * ) noexcept { return "string";  }
        const std::string typeString(int          ) noexcept { return "number";  }
        const std::string typeString(float        ) noexcept { return "number";  }
        const std::string typeString(bool         ) noexcept { return "boolean"; }
        template <class P>
        const std::string typeString(P *          ) noexcept { return "userdata"; }
        /* default: */
        template <class Other>
        const std::string typeString(Other        ) noexcept { return "unknown"; }
        /* } */

        const std::string formatArgs() noexcept {
            return "";
        }

        template <class Head>
        const std::string formatArgs(Head&& head) noexcept {
            return typeString(head);
        }

        template <class Head, class... Atoms>
        const std::string formatArgs(Head&& head, Atoms&&... args) noexcept {
            return typeString(head) + ", " + formatArgs(args...);
        }

        const std::string formatArgsLuaH(int idx) noexcept {
            int tnum = lua_type(L, idx);
            if (tnum >= 0 && tnum < LUA_NUMTAGS)
                return lua_type_names[tnum];
            else
                return "unknown";
        }

        const std::string formatArgsLua(int idx) noexcept {
            if (idx == 0) {
                return "";
            } else if (idx == -1) {
                return formatArgsLuaH(idx);
            } else {
                return formatArgsLuaH(idx) + ", " + formatArgsLua(idx+1);
            }
        }

        /** Wrap a function so that it may be called from Lua.
         * The resulting object can be called after setState()
         * has been called. The returned function will then receive
         * all the Lua values from the stack, and push the return
         * values when called. The function returns the amount
         * of values that were pushed onto the Lua stack.
         *
         * Before the call actually happens however, the arguments
         * are checked. If there is a mismatch between the argument
         * types expected by the C++ function and the types given
         * by the Lua call, then an error message in the following
         * form will be raised:
         *   Class::method : expected (type...) got (type...)
         *
         * Note that the function returned from wrap() cannot
         * be called directly from Lua as it will not have C-linkage.
         * You need to wrap this function inside an extern "C"
         * function.
         *
         * See LUA_METHOD_BIND for a usage example of this function.
         */
        template <class Fn, class... Atoms>
        inline auto wrap(const std::string& fn_name, Fn f, Atoms... atoms) noexcept {
            std::string   errstr  = fn_name + " : expected (" + formatArgs(atoms...) + ")";
            constexpr int idx     = -varargLength(atoms...);
            auto          wrapped = callFromLuaFunction<decltype(f(atoms...))>(idx, f, atoms...);

            return [this, errstr, wrapped, atoms...]() -> int {
                if (!checkArgs(idx, atoms...)) {
                    std::string nerr  = errstr + " got (";
                    nerr             += formatArgsLua(-lua_gettop(L));
                    nerr             += ")";
                    luaL_error(L, nerr.c_str());
                }
                return wrapped();
            };
        }
    };

    // Does not need to be initialized, is simply a magic number used for run-time
    // type-checking in Lua
    extern std::atomic<uint64_t> id_incr;

    template <class T>
    struct LuaPtr {
    private:

    public:
        T *ptr;
        uint64_t type_id;

        explicit LuaPtr(T *ptr) {
            type_id   = id_incr++;
            this->ptr = ptr;
        }

        void provide(lua_State *L) const {
            struct LuaPtr *ud = (struct LuaPtr *)lua_newuserdata(L, sizeof(LuaPtr<T>));
            memcpy(ud, this, sizeof(*ud));
        }
    };

    template <class T>
    class LuaIface {
    private:
        const luaL_Reg *regs;
        T              *inst;
        LuaPtr<T>       lua_ptr;

    public:
        LuaIface(T *inst, const luaL_Reg *regs) : lua_ptr(inst) {
            this->regs = regs;
            this->inst = inst;
        }

        virtual ~LuaIface() {}

        virtual void luaOpen(lua_State *L, const char *name) {
            // Provide pointer to Lua
            lua_ptr.provide(L);

            // Metatable for pointer
            lua_newtable(L);

            // Set methods to be indexed
            lua_pushstring(L, "__index");

            // Push method table
            luaL_checkversion(L);
            lua_createtable(L, 0, 32);
            luaL_setfuncs(L, regs, 0);

            lua_settable(L, -3);

            // Set metatable for pointer userdata
            lua_setmetatable(L, -2);

            lua_setglobal(L, name);
        }
    };

    bool isCallable(lua_State *L, int idx);

    template <int num> constexpr int countT() {
        return num;
    }
    template <int num, class, class... T> constexpr int countT() {
        return countT<num+1, T...>();
    }
    template <class... T> constexpr int countT() {
        return countT<0, T...>();
    }

    /** C++ bindings to make the Lua API easier to deal with.
     */
    class Script {
    private:
        lua_State *L;
        bool enabled = true;

    public:
        std::string src;
        std::string abs_src;

        /** Initialize a Lua state and load a script.
         * 
         * @param path Path to the Lua script.
         */
        explicit Script(std::string path);

        /** Initialize a Lua state. */
        Script();

        /** Destroy a Lua state. */
        virtual ~Script() noexcept;

        /** Get the raw Lua state. */
        lua_State *getL() noexcept;

        /** Load a script into the Lua state.
         *
         * @param path Path to the Lua script. 
         */
        virtual void from(const std::string& path);

        /** Open a Lua interface in the Script. */
        template <class T>
        void open(LuaIface<T> *iface, std::string name) {
            iface->luaOpen(L, name.c_str());
        }

        int call_r(int n) noexcept {
            return n;
        }

        template <class A>
        int call_r(int n, A arg) noexcept {
            luaPush(L, arg);
            return n+1;
        }

        /** Utility function for Script::call in order to push arguments
         *  onto the Lua stack. */
        template <class A, class... Arg>
        int call_r(int n, A arg, Arg... args) noexcept {
            luaPush(L, arg);
            return call_r(n+1, args...);
        }

        template <int idx>
        std::tuple<> ret_r() noexcept {
            return std::make_tuple();
        }

        /** Utility function for Script::call in order to retrieve returned
         *  values as a tuple. */
        template <int idx, class A, class... Arg>
        std::tuple<A, Arg...> ret_r() noexcept {
            return std::tuple_cat(
                std::tuple<A>(LuaValue<A>().get(L, idx)),
                ret_r<idx-1, Arg...>()
            );
        }

        /** Call a global Lua function and return the result(s).
         *
         * @tparam T... The list of return types.
         * @tparam Arg... The list of function argument types.
         * @param name Name of global Lua function.
         * @param args Any number of arguments that the Lua function requires.
         */
        template <class... T, class... Arg>
        std::tuple<T...> call(std::string name, Arg... args) {
            lua_pushcfunction(L, hwk_lua_error_handler_callback);
            lua_getglobal(L, name.c_str());
            if (!isCallable(L, -1))
                throw LuaError("Unable to retrieve " + name + " function from Lua state");
            int nargs = call_r(0, args...);
            constexpr int nres = countT<T...>();

            // -n-nargs is the position of hwk_lua_error_handler_callback
            // on the Lua stack.
            if (lua_pcall(L, nargs, nres, -2-nargs) != LUA_OK) {
                auto exc = std::unique_ptr<LuaError>(static_cast<LuaError*>(
                    // Here be dragons
                    lua_touserdata(L, -1)
                ));
                if (!exc)
                    throw LuaError("Unknown error");
                LuaError err = *exc;
                throw err;
            }
            return ret_r<-nres, T...>();
        }

        /** Retrieve a global Lua value. */
        template <class T>
        T get(std::string name);

        /** Set a global Lua value.
         * 
         * @param name Name of global variable.
         * @param value Value to set the variable to.
         */
        template <class T>
        void set(std::string name, T value) {
            if (luaPush(L, value) == 1) {
                lua_setglobal(L, name.c_str());
            }
        }

        void toggle(bool enabled) noexcept;

        inline bool isEnabled() noexcept {
            return enabled;
        }

        inline void setEnabled(bool enabled) noexcept {
            this->enabled = enabled;
        }

        /** Run Lua code inside the Lua state.
         *
         * @param str Lua code. 
         */
        void exec(const std::string& str);

        /** Reset the Lua state, will destroy all data currently
         *  held within it */
        void reset();

        /** Reload from the file that the Lua state was initially
         *  initialized with. */
        void reload();
    };
}

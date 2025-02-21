// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "Luau/Common.h"

// Analysis files
#include "Luau/Frontend.h"
#include "Fixture.h"


#include <string>

#include <string.h>

static void setupState(lua_State* L)
{
    luaL_openlibs(L);

    luaL_sandbox(L);
}

static std::string runCode(lua_State* L, const std::string& source)
{
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source.data(), source.length(), nullptr, &bytecodeSize);
    int result = luau_load(L, "=stdin", bytecode, bytecodeSize, 0);
    free(bytecode);

    if (result != 0)
    {
        size_t len;
        const char* msg = lua_tolstring(L, -1, &len);

        std::string error(msg, len);
        lua_pop(L, 1);

        return error;
    }

    lua_State* T = lua_newthread(L);

    lua_pushvalue(L, -2);
    lua_remove(L, -3);
    lua_xmove(L, T, 1);

    int status = lua_resume(T, NULL, 0);

    if (status == 0)
    {
        int n = lua_gettop(T);

        if (n)
        {
            luaL_checkstack(T, LUA_MINSTACK, "too many results to print");
            lua_getglobal(T, "print");
            lua_insert(T, 1);
            lua_pcall(T, n, 0, 0);
        }

        lua_pop(L, 1); // pop T
        return std::string();
    }
    else
    {
        std::string error;

        lua_Debug ar;
        if (lua_getinfo(L, 0, "sln", &ar))
        {
            error += ar.short_src;
            error += ':';
            error += std::to_string(ar.currentline);
            error += ": ";
        }

        if (status == LUA_YIELD)
        {
            error += "thread yielded unexpectedly";
        }
        else if (const char* str = lua_tostring(T, -1))
        {
            error += str;
        }

        error += "\nstack backtrace:\n";
        error += lua_debugtrace(T);

        lua_pop(L, 1); // pop T
        return error;
    }
}

extern "C" const char* executeScript(const char* source)
{
    // setup flags
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;

    // create new state
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();

    // setup state
    setupState(L);

    // sandbox thread
    luaL_sandboxthread(L);

    // static string for caching result (prevents dangling ptr on function exit)
    static std::string result;

    // run code + collect error
    result = runCode(L, source);

    return result.empty() ? NULL : result.c_str();
}



// Analysis

namespace Luau
{


struct TestFileResolver
    : FileResolver
    , ModuleResolver
{
    std::optional<ModuleInfo> resolveModuleInfo(const ModuleName& currentModuleName, const AstExpr& pathExpr) override;

    const ModulePtr getModule(const ModuleName& moduleName) const override;

    bool moduleExists(const ModuleName& moduleName) const override;

    std::optional<SourceCode> readSource(const ModuleName& name) override;

    std::optional<ModuleInfo> resolveModule(const ModuleInfo* context, AstExpr* expr) override;

    std::string getHumanReadableModuleName(const ModuleName& name) const override;

    std::optional<std::string> getEnvironmentForModule(const ModuleName& name) const override;

    std::unordered_map<ModuleName, std::string> source;
    std::unordered_map<ModuleName, SourceCode::Type> sourceTypes;
    std::unordered_map<ModuleName, std::string> environments;
};

struct TestConfigResolver : ConfigResolver
{
    Config defaultConfig;
    std::unordered_map<ModuleName, Config> configFiles;

    const Config& getConfig(const ModuleName& name) const override;
};

struct NullModuleResolver : ModuleResolver
{
    std::optional<ModuleInfo> resolveModuleInfo(const ModuleName& currentModuleName, const AstExpr& pathExpr) override
    {
        return std::nullopt;
    }
    const ModulePtr getModule(const ModuleName& moduleName) const override
    {
        return nullptr;
    }
    bool moduleExists(const ModuleName& moduleName) const override
    {
        return false;
    }
    std::string getHumanReadableModuleName(const ModuleName& moduleName) const override
    {
        return moduleName;
    }
};

Frontend frontend;
TestFileResolver fileResolver;
TestConfigResolver configResolver;
NullModuleResolver moduleResolver;

CheckResult frontendCheck(Mode mode, const std::string& source, std::optional<FrontendOptions> options)
{
    ModuleName mm = fromString(mainModuleName);
    configResolver.defaultConfig.mode = mode;
    fileResolver.source[mm] = std::move(source);
    frontend.markDirty(mm);

    CheckResult result = frontend.check(mm, options);

    return result;
}

static std::string runAnalysis(const std::string& source)
{
    const std::string strResult;

    CheckResult checkResult = frontendCheck(Luau::Mode::Strict, source);

    // Collect errors
    for (auto error : checkResult.errors)
    {
        strResult += error.getMessage();
    }

    return strResult;
}

} // namespace Luau

extern "C" const char* executeAnalysis(const char* source)
{
    // setup flags
    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;


    // static string for caching result (prevents dangling ptr on function exit)
    static std::string result;

    // run
    result = Luau::runAnalysis(source);

    return result.empty() ? NULL : result.c_str();
}
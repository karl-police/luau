// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lua.h"
#include "lualib.h"

#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Parser.h"
#include "Luau/BytecodeSummary.h"
#include "FileUtils.h"
#include "Flags.h"

#include "lobject.h"

#include <memory>

static void displayHelp(const char* argv0)
{
    printf("Usage: %s [options] [file list]\n", argv0);

    exit(0);
}
    


int main() {
    auto name = "";

    std::optional<std::string> source = readFile(name);

    if (!source)
    {
        fprintf(stderr, "Error opening %s\n", name);
        return false;
    }

    const std::string& bytecode = *source;

    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();

    luau_load(L, name, bytecode.data(), bytecode.size(), 0);

    
    Luau::BytecodeBuilder bcb;

    
   

    return 0;
}

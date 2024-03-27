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
#include "Luau/IrBuilder.h"
#include "CodeGenLower.h"

#include "lua.h"
#include "lapi.h"
#include "lobject.h"
#include "lstate.h"

#include <memory>

static void displayHelp(const char* argv0)
{
    printf("Usage: %s [options] [file list]\n", argv0);

    exit(0);
}



class BytecodeParser
{
public:
    Luau::BytecodeBuilder bcb;


    void parseProto(Proto* proto);
};



void BytecodeParser::parseProto(Proto* proto)
{
    for (int i = 0; i < proto->sizecode; i++)
    {
        Instruction insn = proto->code[i];
        uint8_t op = LUAU_INSN_OP(insn);


        switch (op)
        {
        case LOP_LOADNIL:
        {
            bcb.addConstantNil();
            break;
        }
        case LOP_LOADK:
        {
            int ra = LUAU_INSN_A(insn);
            int kb = LUAU_INSN_D(insn);
            auto idk = proto->k[kb];

            break;
        }
        case LOP_RETURN:
        {
            int nresults = LUAU_INSN_B(insn) - 1;
        }
        default:
        {

            break;
        }
        }
    }
}




int main() {
    auto name = "D:\\Dokumente\\Rojo\\AnimationClipEditor\\src\\Src\\Components\\AnimationClipEditorPlugin.luac";

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

    CODEGEN_ASSERT(lua_isLfunction(L, -1));
    const TValue* func = luaA_toobject(L, -1);

    Proto* root = clvalue(func)->l.p;

    std::vector<Proto*> protos;
    Luau::CodeGen::gatherFunctions(protos, root, Luau::CodeGen::CodeGen_ColdFunctions);
    
    

    BytecodeParser bytecodeParser;

    for (size_t i = 0; i < protos.size(); i++)
    {
        Proto* proto = protos[i];

        bytecodeParser.parseProto(proto);
    }
   

    return 0;
}

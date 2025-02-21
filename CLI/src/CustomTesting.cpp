#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "Luau/Flags.h"
#include "Luau/Common.h"
#include "Luau/ExperimentalFlags.h"
#include "Luau/Frontend.h"

#include <string>
#include <string.h>

namespace Luau
{
static std::vector<std::string_view> parsePathExpr(const AstExpr& pathExpr)
{
    const AstExprIndexName* indexName = pathExpr.as<AstExprIndexName>();
    if (!indexName)
        return {};

    std::vector<std::string_view> segments{indexName->index.value};

    while (true)
    {
        if (AstExprIndexName* in = indexName->expr->as<AstExprIndexName>())
        {
            segments.push_back(in->index.value);
            indexName = in;
            continue;
        }
        else if (AstExprGlobal* indexNameAsGlobal = indexName->expr->as<AstExprGlobal>())
        {
            segments.push_back(indexNameAsGlobal->name.value);
            break;
        }
        else if (AstExprLocal* indexNameAsLocal = indexName->expr->as<AstExprLocal>())
        {
            segments.push_back(indexNameAsLocal->local->name.value);
            break;
        }
        else
            return {};
    }

    std::reverse(segments.begin(), segments.end());
    return segments;
}


std::optional<std::string> pathExprToModuleName(const ModuleName& currentModuleName, const std::vector<std::string_view>& segments)
{
    if (segments.empty())
        return std::nullopt;

    std::vector<std::string_view> result;

    auto it = segments.begin();

    if (*it == "script" && !currentModuleName.empty())
    {
        result = split(currentModuleName, '/');
        ++it;
    }

    for (; it != segments.end(); ++it)
    {
        if (result.size() > 1 && *it == "Parent")
            result.pop_back();
        else
            result.push_back(*it);
    }

    return join(result, "/");
}

std::optional<std::string> pathExprToModuleName(const ModuleName& currentModuleName, const AstExpr& pathExpr)
{
    std::vector<std::string_view> segments = parsePathExpr(pathExpr);
    return pathExprToModuleName(currentModuleName, segments);
}

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

std::optional<ModuleInfo> TestFileResolver::resolveModuleInfo(const ModuleName& currentModuleName, const AstExpr& pathExpr)
{
    if (auto name = pathExprToModuleName(currentModuleName, pathExpr))
        return {{*name, false}};

    return std::nullopt;
}

const ModulePtr TestFileResolver::getModule(const ModuleName& moduleName) const
{
    LUAU_ASSERT(false);
    return nullptr;
}

bool TestFileResolver::moduleExists(const ModuleName& moduleName) const
{
    auto it = source.find(moduleName);
    return (it != source.end());
}

std::optional<SourceCode> TestFileResolver::readSource(const ModuleName& name)
{
    auto it = source.find(name);
    if (it == source.end())
        return std::nullopt;

    SourceCode::Type sourceType = SourceCode::Module;

    auto it2 = sourceTypes.find(name);
    if (it2 != sourceTypes.end())
        sourceType = it2->second;

    return SourceCode{it->second, sourceType};
}

std::optional<ModuleInfo> TestFileResolver::resolveModule(const ModuleInfo* context, AstExpr* expr)
{
    if (AstExprGlobal* g = expr->as<AstExprGlobal>())
    {
        if (g->name == "game")
            return ModuleInfo{"game"};
        if (g->name == "workspace")
            return ModuleInfo{"workspace"};
        if (g->name == "script")
            return context ? std::optional<ModuleInfo>(*context) : std::nullopt;
    }
    else if (AstExprIndexName* i = expr->as<AstExprIndexName>(); i && context)
    {
        if (i->index == "Parent")
        {
            std::string_view view = context->name;
            size_t lastSeparatorIndex = view.find_last_of('/');

            if (lastSeparatorIndex == std::string_view::npos)
                return std::nullopt;

            return ModuleInfo{ModuleName(view.substr(0, lastSeparatorIndex)), context->optional};
        }
        else
        {
            return ModuleInfo{context->name + '/' + i->index.value, context->optional};
        }
    }
    else if (AstExprIndexExpr* i = expr->as<AstExprIndexExpr>(); i && context)
    {
        if (AstExprConstantString* index = i->index->as<AstExprConstantString>())
        {
            return ModuleInfo{context->name + '/' + std::string(index->value.data, index->value.size), context->optional};
        }
    }
    else if (AstExprCall* call = expr->as<AstExprCall>(); call && call->self && call->args.size >= 1 && context)
    {
        if (AstExprConstantString* index = call->args.data[0]->as<AstExprConstantString>())
        {
            AstName func = call->func->as<AstExprIndexName>()->index;

            if (func == "GetService" && context->name == "game")
                return ModuleInfo{"game/" + std::string(index->value.data, index->value.size)};
        }
    }

    return std::nullopt;
}

std::string TestFileResolver::getHumanReadableModuleName(const ModuleName& name) const
{
    // We have a handful of tests that need to distinguish between a canonical
    // ModuleName and the human-readable version so we apply a simple transform
    // here:  We replace all slashes with dots.
    std::string result = name;
    for (size_t i = 0; i < result.size(); ++i)
    {
        if (result[i] == '/')
            result[i] = '.';
    }

    return result;
}

std::optional<std::string> TestFileResolver::getEnvironmentForModule(const ModuleName& name) const
{
    auto it = environments.find(name);
    if (it != environments.end())
        return it->second;

    return std::nullopt;
}

const Config& TestConfigResolver::getConfig(const ModuleName& name) const
{
    auto it = configFiles.find(name);
    if (it != configFiles.end())
        return it->second;

    return defaultConfig;
}

TestFileResolver fileResolver;
TestConfigResolver configResolver;
NullModuleResolver moduleResolver;

CheckResult frontendCheck(Mode mode, const std::string& source, std::optional<FrontendOptions> options)
{
    Luau::Frontend frontend(&fileResolver, &configResolver);

    ModuleName mm = "web";
    configResolver.defaultConfig.mode = mode;
    fileResolver.source[mm] = std::move(source);
    frontend.markDirty(mm);

    CheckResult result = frontend.check(mm, options);

    return result;
}

static std::string runAnalysis(const std::string& source)
{
    std::string strResult;

    CheckResult checkResult = frontendCheck(Mode::Strict, source, std::nullopt);

    // Collect errors
    for (auto error : checkResult.errors)
    {
        strResult += toString(error) += "\n";
    }

    return strResult;
}

} // namespace Luau

const char* executeAnalysis(const char* source)
{
    // static string for caching result (prevents dangling ptr on function exit)
    static std::string result;

    // run
    result = Luau::runAnalysis(source);

    return result.empty() ? NULL : result.c_str();
}

int main(int argc, char** argv)
{
    setLuauFlagsDefault();

    for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
    {
        if (strncmp(flag->name, "Luau", 4) == 0)
        {
            flag->value = true;
            printf("%s\n", flag->name);
        }
    }
       

    executeAnalysis(R"(
    --!strict
type function typeFunction(t)
    print("Result:")
    print(t)
    print(t:value())
    print(t.tag)
    print("Is Number?:", t:is("number"))
    print("Is String?:", t:is("string"))

    return t
end

type a = typeFunction<"Hello World">
)");


    return 0;
}
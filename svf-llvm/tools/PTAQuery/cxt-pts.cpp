#include "DDA/ContextDDA.h"
#include "DDA/DDAClient.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/SourceMgr.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace SVF;
using namespace llvm;

namespace
{

std::string findExtAPIPath()
{
    if (const char* svfPath = std::getenv("SVF_PATH"))
    {
        std::filesystem::path candidate = std::filesystem::path(svfPath) / ".." / "lib" / "extapi.bc";
        if (std::filesystem::exists(candidate))
            return candidate.lexically_normal().string();
    }

    const std::vector<std::filesystem::path> fallbacks = {
        std::filesystem::path("Release-build/lib/extapi.bc"),
        std::filesystem::path("Debug-build/lib/extapi.bc"),
        std::filesystem::path("../SVF/Release-build/lib/extapi.bc"),
        std::filesystem::path("../SVF/Debug-build/lib/extapi.bc")
    };

    for (const auto& candidate : fallbacks)
    {
        if (std::filesystem::exists(candidate))
            return candidate.lexically_normal().string();
    }

    return "";
}

std::string renderSVFVar(const SVFVar* var)
{
    std::string text = var->toString();
    for (char& ch : text)
    {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
    }
    return text;
}

std::string renderValueName(const Value* value)
{
    if (value == nullptr)
        return "(unknown)";

    if (const auto* allocaInst = dyn_cast<AllocaInst>(value))
    {
#if LLVM_VERSION_MAJOR > 16
        for (llvm::DbgVariableIntrinsic* dbgInst : llvm::findDbgDeclares(const_cast<AllocaInst*>(allocaInst)))
#else
        for (llvm::DbgVariableIntrinsic* dbgInst : FindDbgDeclareUses(const_cast<AllocaInst*>(allocaInst)))
#endif
        {
            if (const auto* dbgDeclare = dyn_cast<DbgDeclareInst>(dbgInst))
            {
                const llvm::DIVariable* divar = dbgDeclare->getVariable();
                if (divar != nullptr && !divar->getName().empty())
                    return divar->getName().str();
            }
        }
    }

    std::string name = value->getName().str();
    if (!name.empty())
        return name;

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    if (llvmModuleSet->hasValueNode(value))
        return renderSVFVar(SVFIR::getPAG()->getSVFVar(llvmModuleSet->getValueNode(value)));

    return "(unnamed)";
}

const llvm::DIVariable* getDebugVariableForAlloca(const AllocaInst* allocaInst)
{
    if (allocaInst == nullptr)
        return nullptr;

#if LLVM_VERSION_MAJOR > 16
    for (llvm::DbgVariableIntrinsic* dbgInst : llvm::findDbgDeclares(const_cast<AllocaInst*>(allocaInst)))
#else
    for (llvm::DbgVariableIntrinsic* dbgInst : FindDbgDeclareUses(const_cast<AllocaInst*>(allocaInst)))
#endif
    {
        if (const auto* dbgDeclare = dyn_cast<DbgDeclareInst>(dbgInst))
            return dbgDeclare->getVariable();
    }

    return nullptr;
}

const llvm::DIType* unwrapDebugType(const llvm::DIType* type)
{
    const llvm::DIType* current = type;
    while (const auto* derived = dyn_cast_or_null<DIDerivedType>(current))
    {
        if (derived->getBaseType() == nullptr)
            break;
        current = derived->getBaseType();
    }
    return current;
}

std::string getStructFieldName(const AllocaInst* allocaInst, const GEPOperator* gep)
{
    const llvm::DIVariable* divar = getDebugVariableForAlloca(allocaInst);
    if (divar == nullptr)
        return "";

    const llvm::DIType* type = unwrapDebugType(divar->getType());
    const auto* composite = dyn_cast_or_null<DICompositeType>(type);
    if (composite == nullptr)
        return "";

    const DINodeArray elements = composite->getElements();
    if (elements.empty())
        return "";

    auto indexIt = gep->idx_begin();
    if (indexIt == gep->idx_end())
        return "";
    ++indexIt; // skip the leading aggregate index
    if (indexIt == gep->idx_end())
        return "";

    const auto* fieldIndex = dyn_cast<ConstantInt>(indexIt->get());
    if (fieldIndex == nullptr)
        return "";

    const auto fieldNumber = static_cast<std::size_t>(fieldIndex->getZExtValue());
    if (fieldNumber >= elements.size())
        return "";

    const auto* member = dyn_cast<DIDerivedType>(elements[fieldNumber]);
    if (member == nullptr || member->getName().empty())
        return "";

    return member->getName().str();
}

std::string renderObjectName(const SVFVar* obj)
{
    if (obj == nullptr)
        return "(unknown)";

    std::string name = obj->getName();
    if (!name.empty())
    {
        if (isa<FunObjVar, FunValVar>(obj))
            return name;

        if (const FunObjVar* fun = obj->getFunction())
            return name + "_" + fun->getName();

        return name;
    }

    return renderSVFVar(obj);
}

std::string renderStorageName(const Value* value)
{
    if (value == nullptr)
        return "(unknown)";

    if (isa<AllocaInst>(value) || isa<GlobalVariable>(value))
        return renderValueName(value);

    if (const auto* gep = dyn_cast<GEPOperator>(value))
    {
        std::ostringstream oss;
        const Value* basePointer = gep->getPointerOperand()->stripPointerCasts();
        oss << renderStorageName(basePointer);

        if (const auto* allocaInst = dyn_cast<AllocaInst>(basePointer))
        {
            std::string fieldName = getStructFieldName(allocaInst, gep);
            if (!fieldName.empty())
            {
                oss << "." << fieldName;
                return oss.str();
            }
        }

        bool sawField = false;
        for (auto indexIt = gep->idx_begin(), indexEnd = gep->idx_end(); indexIt != indexEnd; ++indexIt)
        {
            if (const auto* constIndex = dyn_cast<ConstantInt>(indexIt->get()))
            {
                oss << ".field" << constIndex->getSExtValue();
                sawField = true;
            }
        }

        if (!sawField)
            oss << ".gep";
        return oss.str();
    }

    return renderValueName(value);
}

struct PointerVariable
{
    std::string name;
    const Value* storage = nullptr;
    const Function* function = nullptr;
    Type* storedType = nullptr;
    std::vector<const Value*> storedValues;
    const Value* finalValue = nullptr;
    const Value* exitQueryValue = nullptr;
};

struct PointerQuery
{
    std::string name;
    NodeID rhsNodeId = 0;
    ContextCond context;
};

std::vector<PointerVariable> collectPointerVariables(Module& module)
{
    std::vector<PointerVariable> variables;
    std::map<const Value*, std::size_t> indexByStorage;

    for (Function& function : module)
    {
        if (function.isDeclaration())
            continue;

        for (Instruction& inst : instructions(function))
        {
            const auto* store = dyn_cast<StoreInst>(&inst);
            if (store == nullptr || !store->getValueOperand()->getType()->isPointerTy())
                continue;

            const Value* storage = store->getPointerOperand();
            auto found = indexByStorage.find(storage);
            if (found == indexByStorage.end())
            {
                PointerVariable variable;
                variable.name = renderStorageName(storage);
                if (!isa<GlobalVariable>(storage))
                    variable.name += "_" + function.getName().str();
                variable.storage = storage;
                variable.function = &function;
                variable.storedType = store->getValueOperand()->getType();
                indexByStorage[storage] = variables.size();
                variables.push_back(variable);
                found = indexByStorage.find(storage);
            }

            if (found == indexByStorage.end())
                continue;

            variables[found->second].storedValues.push_back(store->getValueOperand()->stripPointerCasts());
            variables[found->second].finalValue = store->getValueOperand()->stripPointerCasts();
        }
    }

    for (PointerVariable& variable : variables)
    {
        if (variable.function == nullptr || variable.storedType == nullptr)
            continue;

        if (variable.storedValues.size() <= 1)
            continue;

        ReturnInst* uniqueReturn = nullptr;
        for (BasicBlock& block : *const_cast<Function*>(variable.function))
        {
            auto* ret = dyn_cast<ReturnInst>(block.getTerminator());
            if (ret == nullptr)
                continue;

            if (uniqueReturn != nullptr)
            {
                uniqueReturn = nullptr;
                break;
            }

            uniqueReturn = ret;
        }

        if (uniqueReturn == nullptr)
            continue;

        llvm::IRBuilder<> builder(uniqueReturn);
        variable.exitQueryValue = builder.CreateLoad(
            variable.storedType,
            const_cast<Value*>(variable.storage),
            "__svf_exit_" + variable.name);
    }

    for (PointerVariable& variable : variables)
    {
        if (variable.exitQueryValue != nullptr)
        {
            variable.finalValue = variable.exitQueryValue;
        }
    }

    return variables;
}

ContextCond buildQueryContext(ContextDDA* pta, const Value* value)
{
    ContextCond context;

    const auto* call = dyn_cast<CallBase>(value);
    if (call == nullptr || LLVMUtil::isIntrinsicInst(call))
        return context;

    const Function* calleeFun = call->getCalledFunction();
    if (calleeFun == nullptr)
        return context;

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    const CallICFGNode* callNode = llvmModuleSet->getCallICFGNode(call);
    const FunObjVar* callee = llvmModuleSet->getFunObjVar(calleeFun);
    const CallGraph* callGraph = pta->getCallGraph();

    if (callGraph->hasCallSiteID(callNode, callee))
        context.getContexts().push_back(callGraph->getCallSiteID(callNode, callee));

    return context;
}

std::string renderPointsToSet(SVFIR* pag, const CxtPtSet& pts)
{
    if (pts.empty())
        return "(empty)";

    std::set<std::string> renderedTargets;
    for (const CxtVar& obj : pts)
        renderedTargets.insert(renderObjectName(pag->getSVFVar(obj.get_id())));

    std::ostringstream oss;
    bool first = true;
    for (const std::string& target : renderedTargets)
    {
        if (!first)
            oss << ", ";
        first = false;
        oss << target;
    }
    return oss.str();
}

const Value* resolveQueryValue(
    const Value* value,
    const std::map<std::string, const Value*>& finalValueByStorageName,
    std::set<std::string>& visitedStorages)
{
    const auto* load = dyn_cast_or_null<LoadInst>(value);
    if (load == nullptr)
        return value;

    const std::string storageName = renderStorageName(load->getPointerOperand());
    if (!visitedStorages.insert(storageName).second)
        return value;

    auto found = finalValueByStorageName.find(storageName);
    if (found == finalValueByStorageName.end() || found->second == nullptr)
        return value;

    return resolveQueryValue(found->second, finalValueByStorageName, visitedStorages);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        errs() << "Usage: " << argv[0] << " <input.bc> [additional SVF options]\n";
        errs() << "Runs flow- and context-sensitive DDA and prints: Pointer<TAB>Pointees.\n";
        return 1;
    }

    std::vector<std::string> args = {
        argv[0],
        "-query=all",
        "-cxt",
        "-stat=false",
        "-print-query-pts=false",
        "-print-all-pts=false"
    };

    const std::string extapi = findExtAPIPath();
    if (!extapi.empty())
        args.push_back("-extapi=" + extapi);

    for (int i = 2; i < argc; ++i)
        args.emplace_back(argv[i]);
    args.emplace_back(argv[1]);

    std::vector<char*> optionArgv;
    optionArgv.reserve(args.size());
    for (std::string& arg : args)
        optionArgv.push_back(arg.data());

    std::vector<std::string> moduleNameVec = OptionBase::parseOptions(
        static_cast<int>(optionArgv.size()),
        optionArgv.data(),
        "Flow- and Context-Sensitive Points-to Dumper",
        "[options] <input-bitcode...>");

    if (moduleNameVec.size() != 1)
    {
        errs() << "cxt-pts currently expects exactly one input bitcode file.\n";
        return 1;
    }

    llvm::SMDiagnostic err;
    llvm::LLVMContext llvmContext;
    std::unique_ptr<Module> module = llvm::parseIRFile(moduleNameVec.front(), err, llvmContext);
    if (!module)
    {
        err.print(argv[0], errs());
        return 1;
    }

    std::vector<PointerVariable> variables = collectPointerVariables(*module);
    LLVMModuleSet::buildSVFModule(*module);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    auto client = std::make_unique<DDAClient>();
    std::unique_ptr<ContextDDA> pta = std::make_unique<ContextDDA>(pag, client.get());
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

    pta->initialize();
    std::vector<PointerQuery> queries;
    std::map<std::string, const Value*> finalValueByStorageName;
    for (const PointerVariable& variable : variables)
        finalValueByStorageName[renderStorageName(variable.storage)] = variable.finalValue;

    for (const PointerVariable& variable : variables)
    {
        if (variable.finalValue == nullptr)
            continue;

        std::set<std::string> visitedStorages;
        const Value* queryValue = resolveQueryValue(variable.finalValue, finalValueByStorageName, visitedStorages);
        ContextCond queryContext = buildQueryContext(pta.get(), queryValue);
        NodeID rhsNodeId = 0;

        if (const auto* call = dyn_cast<CallBase>(queryValue))
        {
            if (const Function* calleeFun = call->getCalledFunction())
                rhsNodeId = llvmModuleSet->getReturnNode(calleeFun);
        }

        if (rhsNodeId == 0)
        {
            if (!llvmModuleSet->hasValueNode(queryValue))
                continue;
            rhsNodeId = llvmModuleSet->getValueNode(queryValue);
        }

        const SVFVar* rhsNode = pag->getSVFVar(rhsNodeId);
        if (rhsNode == nullptr || !pag->isValidTopLevelPtr(rhsNode))
            continue;

        PointerQuery query;
        query.name = variable.name;
        query.rhsNodeId = rhsNodeId;
        query.context = std::move(queryContext);
        queries.push_back(std::move(query));
    }

    outs() << "Pointer\tPointees\n";
    for (const PointerQuery& query : queries)
    {
        const CxtPtSet& pts = pta->computeDDAPts(CxtVar(query.context, query.rhsNodeId));
        outs() << query.name << '\t' << renderPointsToSet(pag, pts) << '\n';
    }

    pta.reset();
    client.reset();

    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    llvm_shutdown();
    return 0;
}
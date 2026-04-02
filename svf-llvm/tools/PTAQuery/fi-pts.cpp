#include "WPA/Andersen.h"
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

std::string renderStorageName(const Value* value);

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

const llvm::DIGlobalVariable* getDebugVariableForGlobal(const GlobalVariable* globalVar)
{
    if (globalVar == nullptr)
        return nullptr;

    SmallVector<DIGlobalVariableExpression*, 4> globalExprs;
    globalVar->getDebugInfo(globalExprs);
    for (const auto* expr : globalExprs)
    {
        if (expr == nullptr)
            continue;
        if (const auto* var = expr->getVariable())
            return var;
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

const llvm::DICompositeType* getDebugCompositeTypeForValue(const Value* value)
{
    if (const auto* allocaInst = dyn_cast_or_null<AllocaInst>(value))
    {
        if (const llvm::DIVariable* divar = getDebugVariableForAlloca(allocaInst))
            return dyn_cast_or_null<DICompositeType>(unwrapDebugType(divar->getType()));
    }

    if (const auto* globalVar = dyn_cast_or_null<GlobalVariable>(value))
    {
        if (const llvm::DIGlobalVariable* divar = getDebugVariableForGlobal(globalVar))
            return dyn_cast_or_null<DICompositeType>(unwrapDebugType(divar->getType()));
    }

    return nullptr;
}

std::string getStructFieldName(const Value* baseValue, const GEPOperator* gep)
{
    const auto* composite = getDebugCompositeTypeForValue(baseValue);
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

std::string renderPointerBaseName(const Value* value)
{
    if (value == nullptr)
        return "(unknown)";

    if (const auto* load = dyn_cast<LoadInst>(value))
        return renderStorageName(load->getPointerOperand());

    return renderStorageName(value);
}

bool isGlobalStorage(const Value* value)
{
    if (value == nullptr)
        return false;

    if (isa<GlobalVariable>(value))
        return true;

    if (const auto* gep = dyn_cast<GEPOperator>(value))
        return isGlobalStorage(gep->getPointerOperand()->stripPointerCasts());

    return false;
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

    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
    if (llvmModuleSet->hasLLVMValue(obj))
    {
        const Value* llvmValue = llvmModuleSet->getLLVMValue(obj);
        std::string llvmName = renderValueName(llvmValue);
        if (const FunObjVar* fun = obj->getFunction())
            return llvmName + "_" + fun->getName();
        return llvmName;
    }

    if (const auto* gepObj = SVFUtil::dyn_cast<GepObjVar>(obj))
    {
        std::ostringstream oss;
        oss << renderObjectName(gepObj->getBaseObj()) << ".offset" << gepObj->getConstantFieldIdx();
        return oss.str();
    }

    if (const auto* baseObj = SVFUtil::dyn_cast<BaseObjVar>(obj))
    {
        if (const ICFGNode* icfgNode = baseObj->getICFGNode())
        {
            if (llvmModuleSet->hasLLVMValue(icfgNode))
            {
                const Value* llvmValue = llvmModuleSet->getLLVMValue(icfgNode);
                std::string llvmName = renderValueName(llvmValue);
                if (const FunObjVar* fun = obj->getFunction())
                    return llvmName + "_" + fun->getName();
                return llvmName;
            }
        }
    }

    return renderSVFVar(obj);
}

std::string renderStorageName(const Value* value)
{
    if (value == nullptr)
        return "(unknown)";

    if (isa<AllocaInst>(value) || isa<GlobalVariable>(value))
        return renderValueName(value);

    if (const auto* load = dyn_cast<LoadInst>(value))
        return renderStorageName(load->getPointerOperand());

    if (const auto* gep = dyn_cast<GEPOperator>(value))
    {
        std::ostringstream oss;
        const Value* basePointer = gep->getPointerOperand()->stripPointerCasts();
        oss << renderPointerBaseName(basePointer);

        std::string fieldName = getStructFieldName(basePointer, gep);
        if (!fieldName.empty())
        {
            oss << "." << fieldName;
            return oss.str();
        }

        std::string gepName = gep->getName().str();
        if (!gepName.empty())
        {
            while (!gepName.empty() && std::isdigit(static_cast<unsigned char>(gepName.back())))
                gepName.pop_back();
            if (!gepName.empty())
            {
                oss << "." << gepName;
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
                if (!isGlobalStorage(storage))
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

std::string renderPointsToSet(SVFIR* pag, const PointsTo& pts)
{
    if (pts.empty())
        return "(empty)";

    std::set<std::string> renderedTargets;
    for (NodeID objId : pts)
        renderedTargets.insert(renderObjectName(pag->getSVFVar(objId)));

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
        errs() << "Runs flow-insensitive Andersen analysis and prints: Pointer<TAB>Pointees.\n";
        return 1;
    }

    std::vector<std::string> args = {
        argv[0],
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
        "Flow-Insensitive Points-to Dumper",
        "[options] <input-bitcode...>");

    if (moduleNameVec.size() != 1)
    {
        errs() << "fi-pts currently expects exactly one input bitcode file.\n";
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

    Andersen* pta = AndersenWaveDiff::createAndersenWaveDiff(pag);
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

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
        queries.push_back(std::move(query));
    }

    outs() << "Pointer\tPointees\n";
    for (const PointerQuery& query : queries)
    {
        const PointsTo& pts = pta->getPts(query.rhsNodeId);
        outs() << query.name << '\t' << renderPointsToSet(pag, pts) << '\n';
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    llvm_shutdown();
    return 0;
}

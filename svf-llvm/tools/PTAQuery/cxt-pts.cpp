//===- cxt-pts.cpp -- Context-sensitive points-to dumper -----------------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2022>  <Yulei Sui>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "DDA/ContextDDA.h"
#include "DDA/DDAClient.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

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




bool isSimplePointerOperand(const Value* value)
{
    if (value == nullptr)
        return false;

    const Value* stripped = value->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalVariable>(stripped))
        return true;

    if (const auto* gep = dyn_cast<GEPOperator>(stripped))
    {
        if (!isSimplePointerOperand(gep->getPointerOperand()))
            return false;

        for (auto indexIt = gep->idx_begin(), indexEnd = gep->idx_end(); indexIt != indexEnd; ++indexIt)
        {
            if (!isa<ConstantInt>(indexIt->get()))
                return false;
        }
        return true;
    }

    return false;
}

bool isSafeForContextQuery(const Value* value)
{
    if (value == nullptr)
        return false;

    if (isa<CallBase>(value))
        return true;

    if (const auto* load = dyn_cast<LoadInst>(value))
        return isSimplePointerOperand(load->getPointerOperand());

    return true;
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


struct PointerQuery
{
    u32_t lineNumber = 0;
    std::string name;
    const Value* queryValue = nullptr;
    NodeID rhsNodeId = 0;
};

std::vector<PointerQuery> collectPointerQueries(Module& module)
{
    std::vector<PointerQuery> queries;

    for (Function& function : module)
    {
        if (function.isDeclaration())
            continue;

        for (Instruction& inst : instructions(function))
        {
            const auto* store = dyn_cast<StoreInst>(&inst);
            if (store == nullptr || !store->getValueOperand()->getType()->isPointerTy())
                continue;

            PointerQuery query;
            query.lineNumber = store->getDebugLoc() ? store->getDebugLoc().getLine() : 0;
            query.name = renderStorageName(store->getPointerOperand());
            if (!isGlobalStorage(store->getPointerOperand()))
                query.name += "_" + function.getName().str();
            query.queryValue = store->getValueOperand()->stripPointerCasts();
            queries.push_back(std::move(query));
        }
    }

    return queries;
}

std::string renderPointsToSetFI(SVFIR* pag, const PointsTo& pts)
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
            oss << ' ' ;
        first = false;
        oss << target;
    }
    return oss.str();
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
            oss << ' ';
        first = false;
        oss << target;
    }
    return oss.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        errs() << "Usage: " << argv[0] << " <input.bc> [additional SVF options]\n";
        errs() << "Runs flow- and context-sensitive DDA and prints: line ptr pointee...\n";
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

    std::vector<PointerQuery> queries = collectPointerQueries(*module);
    LLVMModuleSet::buildSVFModule(*module);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    auto client = std::make_unique<DDAClient>();
    std::unique_ptr<ContextDDA> pta = std::make_unique<ContextDDA>(pag, client.get());
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    LLVMModuleSet* llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

    pta->initialize();

    for (PointerQuery& query : queries)
    {
        if (query.queryValue == nullptr)
            continue;

        NodeID rhsNodeId = 0;
        if (const auto* call = dyn_cast<CallBase>(query.queryValue))
        {
            if (const Function* calleeFun = call->getCalledFunction())
                rhsNodeId = llvmModuleSet->getReturnNode(calleeFun);
        }

        if (rhsNodeId == 0)
        {
            if (!llvmModuleSet->hasValueNode(query.queryValue))
                continue;
            rhsNodeId = llvmModuleSet->getValueNode(query.queryValue);
        }

        const SVFVar* rhsNode = pag->getSVFVar(rhsNodeId);
        if (rhsNode == nullptr || !pag->isValidTopLevelPtr(rhsNode))
            continue;

        query.rhsNodeId = rhsNodeId;
    }

    for (const PointerQuery& query : queries)
    {
        if (query.rhsNodeId == 0)
            continue;

        if (isSafeForContextQuery(query.queryValue))
        {
            const CxtPtSet& pts = pta->computeDDAPts(CxtVar(ContextCond(), query.rhsNodeId));
            outs() << query.lineNumber << ' ' << query.name << ' ' << renderPointsToSet(pag, pts) << '\n';
        }
        else
        {
            const PointsTo& pts = ander->getPts(query.rhsNodeId);
            outs() << query.lineNumber << ' ' << query.name << ' ' << renderPointsToSetFI(pag, pts) << '\n';
        }
    }

    pta.reset();
    client.reset();
    AndersenWaveDiff::releaseAndersenWaveDiff();

    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    llvm_shutdown();
    return 0;
}

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
#ifdef __unix__
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

bool isSimpleCallReturnValue(const Value* value)
{
    if (value == nullptr)
        return false;

    const Value* stripped = value->stripPointerCasts();
    if (isa<Argument>(stripped) || isa<AllocaInst>(stripped) || isa<GlobalVariable>(stripped) || isa<CallBase>(stripped) || isa<PHINode>(stripped) || isa<SelectInst>(stripped))
        return true;

    if (const auto* load = dyn_cast<LoadInst>(stripped))
    {
        const Value* loadPtr = load->getPointerOperand()->stripPointerCasts();
        if (isa<AllocaInst>(loadPtr) || isa<GlobalVariable>(loadPtr))
            return true;
    }

    return false;
}

bool hasSafeCalleeReturnShape(const Function* calleeFun)
{
    if (calleeFun == nullptr || calleeFun->isDeclaration())
        return false;

    bool sawReturn = false;
    for (const BasicBlock& block : *calleeFun)
    {
        const auto* ret = dyn_cast<ReturnInst>(block.getTerminator());
        if (ret == nullptr)
            continue;

        sawReturn = true;
        const Value* retVal = ret->getReturnValue();
        if (retVal == nullptr)
            return false;

        if (!isSimpleCallReturnValue(retVal))
            return false;
    }

    return sawReturn;
}

bool isSafeForContextQuery(const Value* value)
{
    const auto* call = dyn_cast_or_null<CallBase>(value);
    if (call == nullptr || LLVMUtil::isIntrinsicInst(call))
        return false;

    const Function* calleeFun = call->getCalledFunction();
    if (calleeFun == nullptr)
        return false;

    return hasSafeCalleeReturnShape(calleeFun);
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
    ContextCond context;
    bool emitOnlyOnChange = false;
};


std::string renderNamedPointerVariable(const Value* storage, const Function& function)
{
    std::string name = renderStorageName(storage);
    if (!isGlobalStorage(storage))
        name += "_" + function.getName().str();
    return name;
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

std::vector<PointerQuery> collectPointerQueries(Module& module)
{
    std::vector<PointerQuery> queries;

    for (Function& function : module)
    {
        if (function.isDeclaration())
            continue;

        for (Instruction& inst : instructions(function))
        {
            const u32_t lineNumber = inst.getDebugLoc() ? inst.getDebugLoc().getLine() : 0;

            const auto* store = dyn_cast<StoreInst>(&inst);
            if (store != nullptr && store->getValueOperand()->getType()->isPointerTy())
            {
                PointerQuery query;
                query.lineNumber = lineNumber;
                query.name = renderNamedPointerVariable(store->getPointerOperand(), function);
                query.queryValue = store->getValueOperand()->stripPointerCasts();
                queries.push_back(std::move(query));
            }

            if (const auto* load = dyn_cast<LoadInst>(&inst))
            {
                if (load->getType()->isPointerTy())
                {
                    PointerQuery readQuery;
                    readQuery.lineNumber = lineNumber;
                    readQuery.name = renderNamedPointerVariable(load->getPointerOperand(), function);
                    readQuery.queryValue = load;
                    readQuery.emitOnlyOnChange = true;
                    queries.push_back(std::move(readQuery));
                }
            }

        }
    }

    return queries;
}

std::string renderPointsToSet(SVFIR* pag, const PointsTo& pts)
{
    if (pts.empty())
        return "";

    std::set<std::string> renderedTargets;
    for (NodeID objId : pts)
        renderedTargets.insert(renderObjectName(pag->getSVFVar(objId)));

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

bool tryComputeContextPtsIsolated(
    ContextDDA* pta,
    SVFIR* pag,
    const ContextCond& context,
    NodeID rhsNodeId,
    std::string& ptsRendered)
{
#ifdef __unix__
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return false;

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0)
    {
        close(pipefd[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::ostringstream oss;
        const CxtPtSet& pts = pta->computeDDAPts(CxtVar(context, rhsNodeId));
        for (const CxtVar& obj : pts)
            oss << obj.get_id() << '\n';

        const std::string payload = oss.str();
        const char* data = payload.data();
        std::size_t remaining = payload.size();
        while (remaining > 0)
        {
            const ssize_t written = write(pipefd[1], data, remaining);
            if (written <= 0)
                break;
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    std::string payload;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        payload.append(buffer, static_cast<std::size_t>(n));
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return false;

    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
        return false;

    std::set<std::string> renderedTargets;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;
        const NodeID objId = static_cast<NodeID>(std::stoul(line));
        renderedTargets.insert(renderObjectName(pag->getSVFVar(objId)));
    }

    if (renderedTargets.empty())
    {
        ptsRendered = "";
        return true;
    }

    std::ostringstream oss;
    bool first = true;
    for (const std::string& target : renderedTargets)
    {
        if (!first)
            oss << ' ';
        first = false;
        oss << target;
    }
    ptsRendered = oss.str();
    return true;
#else
    (void)pta;
    (void)pag;
    (void)context;
    (void)rhsNodeId;
    (void)ptsRendered;
    return false;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        errs() << "Usage: " << argv[0] << " <input.bc> [additional SVF options]\n";
        errs() << "Runs flow- and context-sensitive DDA query solving and prints: line ptr pointee...\n";
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
        query.context = buildQueryContext(pta.get(), query.queryValue);
    }

    bool hasAnyNonZeroLine = false;
    bool hasAnyQueryablePointerStmt = false;
    for (const PointerQuery& query : queries)
    {
        if (query.rhsNodeId != 0)
        {
            hasAnyQueryablePointerStmt = true;
            if (query.lineNumber != 0)
            {
                hasAnyNonZeroLine = true;
                break;
            }
        }
    }
    if (hasAnyQueryablePointerStmt && !hasAnyNonZeroLine)
        outs() << "WARNING: line numbers are 0; compile with -g to preserve debug line info.\n";

    std::map<std::string, std::string> lastPtsByPointer;
    for (const PointerQuery& query : queries)
    {
        if (query.rhsNodeId == 0)
            continue;
        if (query.lineNumber == 0)
            continue;

        std::string ptsRendered;
        if (isSafeForContextQuery(query.queryValue))
        {
            if (!tryComputeContextPtsIsolated(pta.get(), pag, query.context, query.rhsNodeId, ptsRendered))
            {
                const PointsTo& pts = ander->getPts(query.rhsNodeId);
                ptsRendered = renderPointsToSet(pag, pts);
            }
        }
        else
        {
            const PointsTo& pts = ander->getPts(query.rhsNodeId);
            ptsRendered = renderPointsToSet(pag, pts);
        }

        if (query.emitOnlyOnChange)
        {
            auto found = lastPtsByPointer.find(query.name);
            if (found != lastPtsByPointer.end() && found->second == ptsRendered)
                continue;
        }

        outs() << query.lineNumber << ' ' << query.name << ' ' << ptsRendered << '\n';
        lastPtsByPointer[query.name] = ptsRendered;
    }

    pta.reset();
    client.reset();
    AndersenWaveDiff::releaseAndersenWaveDiff();

    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    llvm_shutdown();
    return 0;
}

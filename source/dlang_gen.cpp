
#include "dlang_gen.h"

#include <sstream>
#include <iomanip>
#include <memory>

#include "llvm/ADT/ArrayRef.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Parse/ParseAST.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "iohelpers.h"

#if __cpp_lib_filesystem
namespace fs = std::filesystem;
#elif __cpp_lib_experimental_filesystem
namespace fs = std::experimental::filesystem;
#endif


using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::driver;
using namespace clang::tooling;
using namespace gentool;


thread_local clang::PrintingPolicy *DlangBindGenerator::g_printPolicy;

// D reserved keywords, possible overlaps from C/C++ sources
std::vector<std::string> reservedIdentifiers = {
    "out", "ref", "version", "debug", "mixin", "with", "unittest", "typeof", "typeid", "super", "body",
    "shared", "pure", "package", "module", "inout", "in", "is", "import", "invariant", "immutable", "interface",
    "function", "delegate", "final", "export", "deprecated", "alias", "abstract", "synchronized",
    "byte", "ubyte", "uint", "ushort", "string"
};

const char* MODULE_HEADER =
R"(
import core.stdc.config;
import std.bitmanip : bitfields;
import std.conv : emplace;

bool isModuleAvailable(alias T)() {
    mixin("import " ~ T ~ ";");
    static if (__traits(compiles, mixin(T).stringof))
        return true;
    else
        return false;
}
    
static if (__traits(compiles, isModuleAvailable!"nsgen" )) 
    static import nsgen;

struct CppClassSizeAttr
{
    alias size this;
    size_t size;
}
CppClassSizeAttr cppclasssize(size_t a) { return CppClassSizeAttr(a); }

struct CppSizeAttr
{
    alias size this;
    size_t size;
}
CppSizeAttr cppsize(size_t a) { return CppSizeAttr(a); }

struct CppMethodAttr{}
CppMethodAttr cppmethod() { return CppMethodAttr(); }

struct PyExtract{}
auto pyExtract(string name = null) { return PyExtract(); }

mixin template RvalueRef()
{
    alias T = typeof(this);
    static assert (is(T == struct));

    @nogc @safe
    ref const(T) byRef() const pure nothrow return
    {
        return this;
    }
}

)";

//
// Flattens hierarchy for nested types
//
std::string merge(std::list<const clang::RecordDecl *> &q)
{
    std::stringstream ss;

    for (const auto &item : q)
    {
        ss << item->getName().str();
        ss << "_";
    }

    return ss.str();
}


// De-anonimize provided decl and all sub decls, will set generated identifier to the types
void deanonimizeTypedef(clang::RecordDecl* decl, const std::string_view optName = std::string_view(), int* count = nullptr)
{
    // TODO: This function is just ugly, refactor!
    int counter = 1; // will be appended to nested anonymous entries
    if (!count)
        count = &counter;
    if (!decl->getIdentifier())
    {
        if (optName.empty())
        {
            auto newName = std::string("_anon").append(std::to_string(*count));
            count += 1;
            const auto& newId = decl->getASTContext().Idents.get(newName);
            decl->setDeclName(DeclarationName(&newId));
        }
        else
        {
            const auto& newId = decl->getASTContext().Idents.get(optName.data());
            decl->setDeclName(DeclarationName(&newId));
        }
    }
    for(auto d : decl->decls())
    {
        if (d == decl)
            continue;

        if (auto td = llvm::dyn_cast<TypedefDecl>(d))
        {
            if (!td->getIdentifier()) // no name, add some
            {
                auto newName = std::string("_anon").append(std::to_string(*count));
                *count += 1;
                const auto& newId = td->getASTContext().Idents.get(newName);
                td->setDeclName(DeclarationName(&newId));
            }
            if (auto tdtype = td->getUnderlyingType().getTypePtr())
            {
                if (tdtype->isDependentType())
                    continue;
                if (auto rd = tdtype->getAsRecordDecl() )
                {
                    deanonimizeTypedef(rd);
                }
            }
        }
        else if (auto rec = llvm::dyn_cast<RecordDecl>(d))
        {
            if (!rec->getIdentifier())
            {
                auto newName = std::string("_anon").append(std::to_string(*count));
                *count += 1;
                const auto& newId = rec->getASTContext().Idents.get(newName);
                rec->setDeclName(DeclarationName(&newId));
            }
        }
    }
}


// Replaces C++ token like arrow operator or scope double-colon
void textReplaceArrowColon(std::string& in)
{
    while (true)
    {
        auto pos = in.rfind("::");
        if (pos == std::string::npos)
            pos = in.rfind("->");
            if (pos == std::string::npos)
                break;
        in.replace(pos, 2, ".");
    }
}


std::string intTypeForSize(unsigned bitWidth, bool signed_ = true)
{
    if (bitWidth == 8) return signed_? "byte" : "ubyte";
    if (bitWidth == 16) return signed_? "short" : "ushort";
    if (bitWidth == 32) return signed_? "int" : "uint";
    if (bitWidth == 64) return signed_? "long" : "ulong";
    return signed_? "int" : "uint";
}


class NamespacePolicy_StringList : public NamespacePolicy
{
    DlangBindGenerator* gen;
    void setGenerator(DlangBindGenerator* g) { gen = g; }
    void beginEntry(const clang::Decl* decl, const std::string& extern_)
    {
        auto& out = gen->out;
        std::vector<std::string> nestedNS;
        gen->getJoinedNS(decl->getDeclContext(), nestedNS);

        out << "extern(" << extern_ << ", ";
        auto it = std::rbegin(nestedNS);
        while (it != nestedNS.rend())
        {
            out << "\"" << *it << "\""; if (it != (nestedNS.rend() - 1)) out << ",";
            it++;
        }
        out << ")" << std::endl;
    }
    void finishEntry(const clang::Decl* decl) {}
};


bool hasVirtualMethods(const RecordDecl* rd)
{
    if (!rd)
        return false;
    if (!isa<CXXRecordDecl>(rd))
        return false;

    auto isVirtual = [](const CXXMethodDecl* f) {
        return f->isVirtual() == true;
    };

    const auto rec = cast<CXXRecordDecl>(rd);
    auto found = std::find_if(rec->method_begin(), rec->method_end(), isVirtual) != rec->method_end();
    return found || std::any_of(rec->bases_begin(), rec->bases_end(), [](auto a){ return hasVirtualMethods(a.getType()->getAsRecordDecl()); });
}


bool DlangBindGenerator::isRelevantPath(const std::string_view path)
{
    if (path.size() == 0 || path.compare("<invalid loc>") == 0) 
        return false;

    auto [fullPath, _] = getFSPathPart(path);
    auto file = fs::path(fullPath);
    for (const auto &p : iops->paths)
    {
        auto inpath = fs::canonical(p);
        if (file.string().rfind(inpath.string()) != std::string::npos)
            return true;
    }
    return false;
}


void DlangBindGenerator::setOptions(const InputOptions *inOpt, const OutputOptions *outOpt)
{
    if (inOpt)
    {
        iops = inOpt;
        cppIsDefault = inOpt->standard.rfind("c++") != std::string::npos;
    }
    if (outOpt)
    {
        if (fileOut.is_open())
            fileOut.close();
        fileOut.open(outOpt->path);
        if (std::find(outOpt->extras.begin(), outOpt->extras.end(), "attr-nogc") != outOpt->extras.end())
            nogc = true;

        // TODO: select valid policy here
        //nsPolicy.reset(new NamespacePolicy_StringList());
    }

    if (!nsPolicy)
        nsPolicy = std::make_unique<NamespacePolicy_StringList>();
    nsPolicy->setGenerator(this);
}


void DlangBindGenerator::prepare()
{
    mixinTemplateId = 1;
    out << MODULE_HEADER;

    out << std::endl;
}


void DlangBindGenerator::finalize()
{
}

void DlangBindGenerator::onMacroDefine(const clang::Token* name, const clang::MacroDirective* macro)
{
    static const int LARGE_MACRO_NUM_TOKENS = 50;

    if (!macro)
        return;

    auto path = macro->getLocation().printToString(*SourceMgr);
    if (!isRelevantPath(path))
        return;

    if (auto mi = macro->getMacroInfo())
    {
        if (mi->isUsedForHeaderGuard() || mi->getNumTokens() == 0)
            return;
        
        if (!name || !name->getIdentifierInfo())
            return;

        auto id = name->getIdentifierInfo()->getName();
        if ( macroDefs.find(id.str()) != macroDefs.end() )
            return;
        else macroDefs.insert(std::make_pair(id.str(), true));

        // indicates that macro probably a simple value staring with minus
        bool tokWithMinus = mi->getNumTokens() == 2 && mi->getReplacementToken(0).getKind() == clang::tok::minus;
        bool prevHash = false; // is the last token was a '#'
        // this measure disallows macros that possibly expands into hundreds of lines
        // it is not accurate in any way, but there is no better way to detect such cases
        if (mi->getNumTokens() == 1 || tokWithMinus)
        {
            // Write commented out macro body if it is function-like
            if (mi->getNumParams())
                out << "/*" << std::endl;
            out << "enum " << name->getIdentifierInfo()->getName().str();
            if (mi->getNumParams())
                out << "(";
            for (auto p : mi->params())
            {
                out << p->getName().str();
                if (p != *(mi->param_end()-1)) out << ", ";
            }
            if (mi->getNumParams())
                out << ")" << std::endl;
            out << " = ";

            for (auto tok : mi->tokens())
            {
                if (tok.isAnyIdentifier())
                {
                    out << tok.getIdentifierInfo()->getName().str() << " "; 
                }
                else if (tok.isLiteral())
                {
                    out << std::string_view(tok.getLiteralData(), tok.getLength()) << " "; 
                }
                else if (auto kw = clang::tok::getKeywordSpelling(tok.getKind()))
                {
                    out << kw << " ";
                }
                else if (auto pu = clang::tok::getPunctuatorSpelling(tok.getKind()))
                { 
                    static const std::vector<clang::tok::TokenKind> wstokens = { // tokens that needs ws after it
                        clang::tok::comma, clang::tok::r_paren, clang::tok::r_brace, clang::tok::semi
                    };
                    bool ws = std::find(wstokens.begin(), wstokens.end(), tok.getKind()) != wstokens.end();
                    out << pu;
                    if (ws) out << " ";
                }
                prevHash = tok.getKind() == clang::tok::hash;
            }
            out << ";";
            // Close commented out function-like macro
            if (mi->getNumParams())
                out << std::endl << "*/";
            out << std::endl;
        }
        else // 'long' macro
        {
            out << "//" << path << std::endl;
            out << "//#define " << name->getIdentifierInfo()->getName().str()
                << " ..." << std::endl;
        }
    }
}

void DlangBindGenerator::onBeginFile(const std::string_view file)
{
    out << std::endl;
    out << "// ------ " << file << std::endl;
    out << std::endl;
}


void DlangBindGenerator::onEndFile(const std::string_view file)
{
}


void DlangBindGenerator::onStructOrClassEnter(const clang::RecordDecl *decl)
{
    classOrStructName = decl->getName().str();
    finalTypeName = merge(declStack).append(classOrStructName);
    declStack.push_back(decl);

    if (!addType(decl, storedTypes))
        return;
    
    const bool hasNamespace = decl->getDeclContext()->isNamespace();
    const auto externStr = externAsString(decl->getDeclContext()->isExternCContext());

    // linkage & namespace
    if (!hasNamespace)
        out << "extern(" << externStr << ")" << std::endl;
    else
        nsPolicy->beginEntry(decl, externStr);


    TypeInfo ti;
    if (!decl->isTemplated()) // skip templated stuff for now
    {
        const auto tdd = decl->getTypeForDecl();
        if (!tdd)
            return;
        if (!decl->isCompleteDefinition())
        {
            // write and close forward decl
            out << "struct " << decl->getName().str() << ";" << std::endl;
            return;
        }
        ti = decl->getASTContext().getTypeInfo(decl->getTypeForDecl());
        if (ti.Width == 0 || ti.Align == 0)
            return;

        // size attr for verifying
        out << "@cppclasssize(" << ti.Width / 8 << ")"
            << " align(" << ti.Align / 8 << ")" << std::endl;
    }

    const auto cxxdecl = llvm::dyn_cast<CXXRecordDecl>(decl);
    bool isDerived = false;
    if (cxxdecl && (cxxdecl->getNumBases() || cxxdecl->getNumVBases()))
        isDerived = true;

    bool isVirtual = hasVirtualMethods(decl);
    if (!isVirtual)
        out << "struct ";
    else if (decl->isUnion())
        out << "union ";
    else //if (decl->isClass() || isDerived)
        out << "class ";

    if (classOrStructName.empty())
    {
        if (auto td = decl->getTypedefNameForAnonDecl())
        {
            classOrStructName = td->getName().str();
            const auto& newId = decl->getASTContext().Idents.get(td->getName());
            const_cast<RecordDecl*>(decl)->setDeclName(DeclarationName(&newId));
        }
        else
        {
            globalAnonTypeId += 1;
            deanonimizeTypedef(
                const_cast<RecordDecl*>(decl), 
                std::string("AnonType_").append(std::to_string(globalAnonTypeId))
            );
            classOrStructName = decl->getName().str();
        }
    }
    else
    {
        // FIXME: OH NO!
        deanonimizeTypedef(const_cast<RecordDecl*>(decl), std::string_view(), &localAnonRecordId);
    }

    out << classOrStructName;
    std::vector<const clang::Decl*> nonvirt;
    if (cxxdecl)
    {
        TemplateDecl* tpd = nullptr;
        const TemplateArgumentList* tparams = nullptr;
        const ClassTemplateSpecializationDecl* tsd = nullptr;
        if (isa<ClassTemplateSpecializationDecl>(decl))
            tsd = cast<ClassTemplateSpecializationDecl>(decl);
        if (tsd)
        {
            tparams = &tsd->getTemplateArgs();
            tpd = tsd->getSpecializedTemplate();
        }
        else
        {
            tpd = cxxdecl->getDescribedClassTemplate();
        }

        if ( decl->isThisDeclarationADefinition() ) // add only actual declarations
        {
            if (tparams)
            {
                out << "(";
                writeTemplateArgs(tparams);
                out << ")";
            }
            else if (tpd)
            {
                out << "(";
                writeTemplateArgs(tpd);
                out << ")";
            }
        }
        // adds list of base classes
        nonvirt = printBases(cxxdecl);
    }
    out << std::endl;
    out << "{" << std::endl;
    {
        IndentBlock _classbody(out, 4);
#if 0
        // Nope, this does not conform to actual layout, 
        // that means we have to apply align on every field instead
        if (!(decl->isUnion() || decl->isTemplated()))
            out << "align(" << ti.Align / 8 << "):" << std::endl;
#endif
        // TODO: precise mix-in where needed on finalize step
        //if (!isVirtual)
        //    out << "mixin RvalueRef;" << std::endl;
        innerDeclIterate(decl);
        int baseid = 0;
        for (auto fakeBase : nonvirt)
        {
            if (!isa<RecordDecl>(fakeBase))
                continue;
            auto brd = cast<RecordDecl>(fakeBase);
            out << brd->getNameAsString() << " ";
            out << "_b" << baseid << ";" << std::endl;
            out << "alias " << "_b" << baseid << " this;" << std::endl;
            baseid+=1;
        }
        fieldIterate(decl);
        if (cxxdecl)
        methodIterate(cxxdecl);
    }
    out << "}" << std::endl;

    if (hasNamespace)
        nsPolicy->finishEntry(decl);
}


void DlangBindGenerator::onStructOrClassLeave(const clang::RecordDecl *decl)
{
    declStack.pop_back();
    if (declStack.empty())
        localAnonRecordId = 1;
    out << std::endl;
}


void DlangBindGenerator::onEnum(const clang::EnumDecl *decl)
{
    if (!addType(decl, enumDecls))
        return;

    const auto size = std::distance(decl->enumerator_begin(), decl->enumerator_end());
    const auto enumTypeString = toDStyle(decl->getIntegerType());

    bool hasName = !decl->getName().empty();
    if (hasName)
    {
        // alias myEnum = int;
        out << "alias " << decl->getName().str() << " = " << enumTypeString << ";" << std::endl;
        // enum : myEnum
        out << "enum "
            << " : " << decl->getName().str() << std::endl;
    }
    else
    {
        out << "enum"
            << " : " << enumTypeString << std::endl;
    }

    out << "{" << std::endl;

    for (const auto e : decl->enumerators())
    {
        out << "    ";
        out << e->getNameAsString() << " = " << e->getInitVal().toString(10, true);
        if (size > 1)
            out << ", ";
        out << std::endl;
    }

    out << "}" << std::endl;
    out << std::endl;
}


void DlangBindGenerator::onFunction(const clang::FunctionDecl *decl)
{
    if (!addType(decl, functionDecls))
        return;

    const auto fn = decl;
    const bool hasNamespace = decl->getDeclContext()->isNamespace();
    const auto externStr = externAsString(decl->getDeclContext()->isExternCContext());

    // linkage & namespace
    if (!hasNamespace)
        out << "extern(" << externStr << ")" << std::endl;
    else
        nsPolicy->beginEntry(decl, externStr);

    // ret type
    {
        const auto typeStr = toDStyle(fn->getReturnType());
        out << typeStr << " ";
    }
    // function name
    out << fn->getName().str();

    if (fn->isTemplated())
    {
        // note that we write to already opened parenthesis
        if (const auto ftd = fn->getDescribedTemplate())
        {
            out << "(";
            writeTemplateArgs(ftd);
            /*
            const auto tplist = ftd->getTemplateParameters();
            for (const auto tp : *tplist)
            {
                out << tp->getName().str();
                if (tp != *(tplist->end() - 1))
                    out << ", ";
            }
            */
            out << ")";
        }
    }

    // argument list
    out << "(";
    writeFnRuntimeArgs(fn);
    out << ")";
    if (nogc)
        out << " @nogc";
    out << ";" << std::endl;

    if (hasNamespace)
        nsPolicy->finishEntry(decl);

    out << std::endl;
}


void DlangBindGenerator::onTypedef(const clang::TypedefDecl *decl)
{
    if (!addType(decl, storedTypes))
        return;

    const auto typedefName = decl->getName().str();
    bool extC = decl->getDeclContext()->isExternCContext();
    bool functionType = decl->getUnderlyingType()->isFunctionType();
    if (decl->getUnderlyingType()->isPointerType())
        functionType = decl->getUnderlyingType()->getPointeeType()->isFunctionType();

    if (auto tdtype = decl->getUnderlyingType().getTypePtr())
    {
        while(tdtype->isPointerType())
            tdtype = tdtype->getPointeeType().getTypePtr();

        auto rd = tdtype->getAsRecordDecl();
        if (rd)
        {
            if(!rd->getIdentifier())
                deanonimizeTypedef(rd, typedefName);
            else
            {
                const auto& newId = rd->getASTContext().Idents.get(typedefName);
                rd->setDeclName(DeclarationName(&newId));
            }
            onStructOrClassEnter(rd);
            onStructOrClassLeave(rd);
            // We're done here, but it also might be an option to append 
            // underscore and add original typedef'ed name as alias
            return; 
        }
    }

    out << "alias " << typedefName << " = ";
    if (functionType)
    {
        out << "extern(" << externAsString(extC) << ") ";
    }
    out << toDStyle(decl->getUnderlyingType()) << ";";
    out << std::endl;

    out << std::endl;
}


void DlangBindGenerator::onGlobalVar(const clang::VarDecl *decl)
{
    if (!addType(decl, storedTypes))
        return;

    bool extC = decl->isExternC();
    bool isExtern = decl->getStorageClass() == StorageClass::SC_Extern;
    bool isStatic = decl->getStorageClass() == StorageClass::SC_Static;

    bool isFnType = decl->getType()->isFunctionType();

    if (isFnType)
    {
        out << "extern(" << externAsString(extC) << ") ";
    }
    if (isExtern)
        out << "extern ";
    if (isStatic)
        out << "__gshared static "; // has no effect on module level, so probably will need to handle this somehow
    out << toDStyle(decl->getType()) << " ";
    out << decl->getName().str() << " ";
    if (const auto init = decl->getInit())
    {
        std::string s;
        llvm::raw_string_ostream os(s);
        printPrettyD(init, os, nullptr, *DlangBindGenerator::g_printPolicy);
        out << " = ";
        // if (decl->getType()->isFloatingType())
        //     out << _adjustVarInit(os.str());
        // else
        //     out << os.str();
        out << os.str();
    }
    out << ";";
    out << std::endl;
}


// adjust initializer for D, for example 50.F -> 50.0f
std::string DlangBindGenerator::_adjustVarInit(const std::string &e)
{
    if (e.length()==0)
      return e;

    std::string res = e;
    if (e.rfind(".F") != std::string::npos)
    {
        res.replace(e.length() - 2, 3, ".0f");
    }
    else if (res.at(res.length() - 1) == 'F')
    {
        res.replace(res.length() - 1, 1, "f");
    }

    return res;
}


// Wraps complex multiple level pointers in parenthesis
std::string DlangBindGenerator::_wrapParens(QualType type)
{
    QualType temp = type;
    std::vector<std::string> parts;
    std::string result;

    // if multilevel pointer, pointer to const, etc...
    if (type->isPointerType() || type->isReferenceType())
    {
        _typeRoll(type, parts);

        if (parts.size() > 0)
        {
            result.clear();
            for (int i = 0; i < parts.size(); i++)
            {
                result.append(parts.at(i));
            }
        }
    }
    else
        result = toDStyle(type);
    return result;
}


// Supposed to recusrsively walk down, and then on walk up write reversed parts
// e.g. const float const * -> const(const(float)*)*
void DlangBindGenerator::_typeRoll(QualType type, std::vector<std::string> &parts)
{
    bool isConst = type.isConstQualified();
    if (type->isReferenceType())
    {
        parts.push_back("ref ");
        type = type->getPointeeType();
        _typeRoll(type, parts);
        return;
    }
    if (isConst)
        parts.push_back("const(");

    if (type->isPointerType())
        _typeRoll(type->getPointeeType(), parts);
    else
    {
        if (type->isReferenceType())
            parts.push_back("ref ");

        if (isConst)
            type.removeLocalConst();

        parts.push_back(toDStyle(type));
        // if (isConst)
        // 	type.addConst();
    }

    // This added complexity is for having matching pairs in parts list
    if (isConst && type->isPointerType())
        parts.push_back(")*");
    else if (isConst)
        parts.push_back(")");
    else if (type->isPointerType())
        parts.push_back("*");
}


std::string DlangBindGenerator::toDStyle(QualType type)
{
    std::string res;
    const auto typeptr = type.getTypePtr();

    if (type->isPointerType() || type->isReferenceType())
    {
        if (type->getPointeeType()->isFunctionType())
            return toDStyle(type->getPointeeType());
        res = _wrapParens(type);
    }
    else if (type->isArrayType())
    {

        if (type->isConstantArrayType())
        {
            if (const auto arr = llvm::dyn_cast<ConstantArrayType>(type))
            {
                std::string _res = toDStyle(arr->getElementType());
                // do pointer replace with 1-sized arrays?
                _res.append("[");
                _res.append(arr->getSize().toString(10, false));
                _res.append("]");
                res = _res;
            }
        }
        else if (const auto arr = llvm::dyn_cast<ArrayType>(type))
        {
            std::string _res = toDStyle(arr->getElementType());
            res.append("[]");
            res = _res;
        }
    }
    else if (type->isFunctionType())
    {
        const auto fp = type->getAs<FunctionProtoType>();

        auto psize = fp->getNumParams();
        const auto ret = fp->getReturnType();
        std::string s;
        s.append(toDStyle(ret));
        s.append(" function(");
        for (const auto p : fp->param_types())
        {
            s.append(toDStyle(p));
            // TODO: extract parameter names from AST?
            //s.append(" ");
            //s.append(p->getName().str());
            if (psize > 1)
            {
                s.append(", ");
                psize -= 1;
            }
        }
        s.append(")");
        res = s;
    }
    else if (type->isBuiltinType())
    {
        res = _toDBuiltInType(type);
    }
    else if (const auto tsp = type->getAs<TemplateSpecializationType>()) // matches MyArray<int> field
    {
        std::string s;
        llvm::raw_string_ostream os(s);
        tsp->getTemplateName().print(os, *DlangBindGenerator::g_printPolicy);
        os << "!"
           << "(";
        //s.clear();
        const auto numArgs = tsp->getNumArgs();
        unsigned int i = 0;
        for (const auto arg : tsp->template_arguments())
        {
            if (i > 0 && i < numArgs)
                os << ", ";
            arg.print(*DlangBindGenerator::g_printPolicy, os);
            i += 1;
        }
        os << ")";
        res = os.str();
    }
    else if (type->isStructureOrClassType() || type->isEnumeralType() || type->isUnionType()) // special case for stripping enum|class|struct keyword
    {
        std::string str;
        if (auto rd = type->getAsRecordDecl())
        {
            str = rd->getName().str();
            #if 0
            // this version will pick nested records skipping namespces,
            // however the function doesn't have optional parameter yet 
            // so we can't choose desired behaviour
            std::vector<std::string> nestedDecl;
            nestedDecl.push_back(rd->getName().str());
            auto ctx = rd->getDeclContext();
            while(!ctx->isTranslationUnit()) 
            {
                if (ctx->isRecord())
                {
                    auto r = cast<RecordDecl>(ctx);
                    nestedDecl.push_back(r->getName().str());
                }
                ctx = ctx->getParent();
            }
            for(auto it = nestedDecl.rbegin(); it != nestedDecl.rend(); it++ )
            {
                if (it != nestedDecl.rbegin())
                    str.append(".");
                str.append(*it);
            }
            #endif
        }
        else 
        {
            // split on whitespace in between "class SomeClass*" and take the part on the right
            str = type.getAsString(*DlangBindGenerator::g_printPolicy);
            auto ws = str.find_first_of(' ', 0);
            if (ws != std::string::npos)
            {
                str = str.substr(ws + 1);
            }
        }
        res = str;
    }
    else if (typeptr && typeptr->isDependentType() && typeptr->getAsRecordDecl())
    {
        if (const auto rec = typeptr->getAsCXXRecordDecl())
        {
            if (const auto dt = rec->getDescribedClassTemplate())
            {
                std::string s;
                llvm::raw_string_ostream os(s);
                os << dt->getName().str();
                os << "!"
                   << "(";
                const auto tplist = dt->getTemplateParameters();
                for (const auto tp : *tplist)
                {
                    os << tp->getName().str();
                    if (tp != *(tplist->end() - 1))
                        os << ", ";
                }
                os << ")";
                res = os.str();
            }
        }
    }
    else
    {
        res = type.getAsString(*DlangBindGenerator::g_printPolicy);
    }

    while (true)
    {
        auto pos = res.rfind("::");
        if (pos == std::string::npos)
            break;
        res.replace(pos, 2, ".");
    }

    return res;
}


std::string DlangBindGenerator::_toDBuiltInType(QualType type)
{
    using TY = BuiltinType::Kind;
    const auto bt = type->getAs<BuiltinType>();
    switch (bt->getKind())
    {
    case TY::Bool:
        return "bool";
    case TY::Char_S:
    case TY::SChar:
        return "char";
    case TY::Char_U:
    case TY::UChar:
        return "ubyte";
    case TY::UShort:
        return "ushort";
    case TY::UInt:
        return "uint";
    case TY::ULong:
        return "cpp_ulong";
    case TY::Long:
        return "cpp_long";
    case TY::ULongLong:
        return "ulong";
    case TY::LongLong:
        return "long";
    default:
        return type.getAsString(*DlangBindGenerator::g_printPolicy);
    }
    assert(bt != nullptr && "clang::BuiltinType expected");
}


std::string DlangBindGenerator::sanitizedIdentifier(const std::string &id)
{
    if (std::find(reservedIdentifiers.begin(), reservedIdentifiers.end(), id) != reservedIdentifiers.end())
        return std::string().append(id).append("_");
    else
        return id;
}


std::string DlangBindGenerator::externAsString(bool isExternC) const
{
    if (cppIsDefault && !isExternC)
    return "C++";
    else
    return "C";
}


// return nested namespaces list for `extern(C++, ...namespace list here...)`
// note that it walks up, so the resulting list is inverted
void DlangBindGenerator::getJoinedNS(const clang::DeclContext *decl, std::vector<std::string> &parts)
{
    if (!decl)
        return;
    if (decl->isTranslationUnit())
        return;
    if (const auto ns = llvm::dyn_cast<NamespaceDecl>(decl))
    {
        parts.push_back(ns->getName().str());
    }

    getJoinedNS(decl->getParent(), parts);
}

std::vector<const clang::Decl*> DlangBindGenerator::printBases(const clang::CXXRecordDecl *decl)
{
    std::vector<const clang::Decl*> nonvirt;
    unsigned int numBases = decl->getNumBases();
    bool first = true;

    // some quirks with array range ret type...
    for (const auto b : decl->bases())
    {
        numBases--;
        Decl* rd = b.getType()->getAsRecordDecl(); 
        if (rd && !hasVirtualMethods(cast<RecordDecl>(rd))){
            nonvirt.push_back(rd);
            continue;
        }
        
        if (first) {
            out << " : ";
            first = false;
        }
        out << toDStyle(b.getType());
        if (numBases)
            out << ", ";
    }

    return nonvirt;
}


void DlangBindGenerator::innerDeclIterate(const clang::RecordDecl *decl)
{
    for (const auto it : decl->decls())
    {
        if (const auto e = llvm::dyn_cast<EnumDecl>(it))
        {
            onEnum(e);
        }
        if (const auto var = llvm::dyn_cast<VarDecl>(it))
        {
            //out << "@cppsize(" << finfo.Width / 8 << ")" << " ";
            out << "static ";
            out << getAccessStr(it->getAccess(), !decl->isClass()) << " ";
            out << toDStyle(var->getType()) << " ";
            out << sanitizedIdentifier(var->getName().str()) << ";";
            out << std::endl;
        }
        if (const auto d = llvm::dyn_cast<RecordDecl>(it))
        {
            if (d == decl || !d->isCompleteDefinition())
                continue;
            onStructOrClassEnter(d);
            onStructOrClassLeave(d);
        }
        if (const auto m = llvm::dyn_cast<FunctionTemplateDecl>(it))
        {
            if (auto fn = m->getAsFunction())
            {
                // skip "problematic" methods and functions for now
                if (!fn->getIdentifier())
                    continue;
                out << getAccessStr(m->getAccess()) << " ";
                onFunction(fn);
            }
        }
        if (isa<TypedefDecl>(it))
        {
            const auto td = cast<TypedefDecl>(it);
            onTypedef(td);
        }
    }
}


void DlangBindGenerator::fieldIterate(const clang::RecordDecl *decl)
{
    // TODO: count in types and alignment for bit fields

    isPrevIsBitfield = false;
    accumBitFieldWidth = 0;
    std::string bitwidthExpr;

    for (const auto it : decl->fields())
    {
        unsigned int bitwidth = 0;

        /*
			mixin(bitfields!(
				uint, "x",    2,
				int,  "y",    3,
				uint, "z",    2,
				bool, "flag", 1));
		*/

        RecordDecl *rec = it->getType()->getAsRecordDecl();

        bool isDependent = it->getType()->isDependentType();
        bool isForwardDecl = true;
        if (rec)
            isForwardDecl = rec->getDefinition() == nullptr;

        TypeInfo finfo;
        if (!isDependent)
        if (!isForwardDecl || it->getType()->isBuiltinType() || it->getType()->isPointerType() || it->getType()->isArrayType())
        {
            finfo = it->getASTContext().getTypeInfo(it->getType()); 
        }

        bool bitfield = it->isBitField();
        if (bitfield)
        {
            if (!isPrevIsBitfield)
            {
                out << "mixin(bitfields!(" << std::endl;
            }
            std::string s;
            llvm::raw_string_ostream os(s);
            //it->getBitWidth()->printPretty(os, nullptr, *DlangBindGenerator::g_printPolicy);
            printPrettyD(it->getBitWidth(), os, nullptr, *DlangBindGenerator::g_printPolicy);
            bitwidthExpr = os.str();
            bitwidth = it->getBitWidthValue(decl->getASTContext());
            accumBitFieldWidth += bitwidth;
        }
        else
        {
            // pad it!
            if (isPrevIsBitfield)
            {
                IndentBlock _indent(out, 4);
                // still on same line
                out << "," << std::endl;

                if (accumBitFieldWidth % 8 != 0)
                    if (accumBitFieldWidth <= 8)
                    {
                        out << "uint, \"\", " << 8 - accumBitFieldWidth;
                    }
                    else if (accumBitFieldWidth <= 16)
                    {
                        out << "uint, \"\", " << 16 - accumBitFieldWidth;
                    }
                    else if (accumBitFieldWidth <= 32)
                    {
                        out << "uint, \"\", " << 32 - accumBitFieldWidth;
                    }
                    else if (accumBitFieldWidth <= 64)
                    {
                        out << "uint, \"\", " << 64 - accumBitFieldWidth;
                    }
                    else
                        assert(0 && "Split it up already!");

                accumBitFieldWidth = 0;

                // close bitfield
                out << "));" << std::endl;
            }
        }

        auto fieldTypeStr = toDStyle(it->getType());
        if (!it->getIdentifier())
        {
            if (fieldTypeStr.compare("_anon") != -1)
            {
                auto n = std::stoi(fieldTypeStr.substr(5));
                const auto& newId = decl->getASTContext().Idents.get(std::string("a") + std::to_string(n) + "_");
                const_cast<FieldDecl*>(it)->setDeclName(DeclarationName(&newId));
            }
        }

        if (bitfield)
        {
            IndentBlock _indent(out, 4);
            if (isPrevIsBitfield) // we are still on same line
                out << "," << std::endl;
            out << fieldTypeStr << ", ";
            out << "\"" << sanitizedIdentifier(it->getName().str()) << "\""
                << ", ";
            out << bitwidthExpr;
        }
        else
        {
            if (finfo.Width)
                out << "@cppsize(" << finfo.Width / 8 << ")"
                    << " ";
            else
                out << "@cppsize(" << 0 << ")"
                    << " ";

            out << getAccessStr(it->getAccess(), !decl->isClass()) << " ";
            out << fieldTypeStr << " ";
            out << sanitizedIdentifier(it->getName().str()) << ";";
            out << std::endl;
        }

        isPrevIsBitfield = bitfield;
    }

    // if we end up with last field being a bit field
    // close bitfield
    if (isPrevIsBitfield)
        out << "));" << std::endl;
}


std::string DlangBindGenerator::getAccessStr(clang::AccessSpecifier ac, bool isStruct /* = false */)
{
    switch (ac)
    {
    case AccessSpecifier::AS_public:
        return "public";
        break;
    case AccessSpecifier::AS_protected:
        return "protected";
        break;
    case AccessSpecifier::AS_private:
        return "private";
        break;
    default:
        return isStruct ? "public" : "private";
    }
}


void DlangBindGenerator::methodIterate(const clang::CXXRecordDecl *decl)
{
    clang::ASTContext *ast = &decl->getASTContext();
    std::unique_ptr<MangleContext> mangleCtx;
    if (ast->getTargetInfo().getTargetOpts().Triple.compare("windows") != std::string::npos)
        mangleCtx.reset(clang::MicrosoftMangleContext::create(*ast, ast->getDiagnostics()));
    else
        mangleCtx.reset(clang::ItaniumMangleContext::create(*ast, ast->getDiagnostics()));

    bool isVirtualDecl = hasVirtualMethods(decl);

    for (const auto m : decl->methods())
    {
        std::string mangledName;
        std::string funcName;
        bool noRetType = false;
        bool isClass = decl->isClass();
        bool hasConstPtrToNonConst = false;
        bool isStatic = m->isStatic();
        bool moveCtor = false;
        bool copyCtor = false;
        bool isCtor = false;
        bool isDefaultCtor = false;
        bool isDtor = false;
        bool isConversionOp = false;
        bool idAssign = false;
        bool disable = false;
        bool customMangle = false;

        // TODO: implicit methods starts at decl position itself, skip anything already listed
        std::string locString = m->getLocStart().printToString(decl->getASTContext().getSourceManager());
        if (std::find_if(storedTypes.begin(), storedTypes.end(), [&locString](const auto& pair) { return pair.first == locString; } ) != storedTypes.end())
        {
            continue;
        }

        // TODO: add policy to be able to deal with generated ctor/move/copy
        if (m->isDefaulted() && !m->isExplicitlyDefaulted())
            continue;

        funcName = m->getNameAsString();
        bool isOperator = m->isOverloadedOperator();
        if (!mangleCtx->shouldMangleDeclName(m))
        {
            mangledName = m->getNameInfo().getName().getAsString();
        }
        else
        {
            if (const auto ct = llvm::dyn_cast<CXXConstructorDecl>(m))
            {
                funcName = "this";
                noRetType = true;
                isCtor = true;
                isDefaultCtor = ct->isDefaultConstructor();
                moveCtor = ct->isMoveConstructor();
                copyCtor = ct->isCopyConstructor();
            }
            else if (auto dt = llvm::dyn_cast<CXXDestructorDecl>(m))
            {
                funcName = "~this";
                noRetType = true;
                isDtor = true;
            }
            else if (!m->isTemplated())
            {
                llvm::raw_string_ostream ostream(mangledName);
                mangleCtx->mangleName(m, ostream);
                ostream.flush();
            }
        }

        if (const auto conv = llvm::dyn_cast<CXXConversionDecl>(m))
        {
            // try remove ref from template argument for conversions
            auto targetType = conv->getConversionType();
            if (targetType->isReferenceType())
                targetType = targetType->getPointeeType();

            const auto tn = toDStyle(targetType);
            funcName = "opCast(Ty:" + tn + ")";
            isConversionOp = true;
        }

        if (isOperator)
        {
            if (m->getOverloadedOperator() == OverloadedOperatorKind::OO_Equal)
            if (m->getNumParams() == 1 
                && m->getReturnType()->isReferenceType()
                && m->parameters()[0]->getType()->getAsRecordDecl() == m->getReturnType()->getAsRecordDecl()
            ){
                idAssign = true;
            }

            // Get and recombine name with template args (if any)
            auto [name, templatedOp, customMangle_] = getOperatorName(m);
            funcName = name + "(" + templatedOp + ")";
            customMangle = customMangle_;
        }

        if (customMangle)
        {
            // due to many little details unfortunately it's not there (yet)
            if (ast->getLangOpts().isCompatibleWithMSVC(LangOptions::MSVC2015))
            {
                const auto pos = funcName.find('(');
                out << "@pyExtract(\"" 
                    <<     decl->getNameAsString() << "::" << m->getNameAsString()
                    << "\")" << "   pragma(mangle, " << "nsgen."
                    <<     decl->getNameAsString() << "_" << std::string_view(funcName.data(), pos) << ".mangleof)"
                    << std::endl;
            }
            else 
            {
                out << "pragma(mangle, \"" << mangledName << "\")" << std::endl;
            }
        }

        if (m->isDefaulted())
            out << "// (default) ";

        

        bool possibleOverride = !(isCtor || isDtor) && m->size_overridden_methods() && m->getBody();
        bool commentOut = (!isVirtualDecl && isDefaultCtor) || idAssign;

        if (!isVirtualDecl && possibleOverride)
            commentOut = true;
        // default ctor for struct not allowed
        if (commentOut)
        {
            out << "// ";
        }

        if (moveCtor)
            out << "// move ctor" << std::endl;
        if (copyCtor)
            out << "// copy ctor" << std::endl;

        if (m->isInlined() && m->hasInlineBody())
        {
            out << "/* inline */ ";
        }

        out << getAccessStr(m->getAccess(), !isClass) << " ";

        if (m->hasAttr<OverrideAttr>() || possibleOverride)
            out << "override ";

        if (isStatic)
            out << "static ";

        if (isClass && !m->isVirtual())
            out << "final ";

        if (moveCtor && m->getAccess() == AccessSpecifier::AS_private)
        {
            out << "@disable ";
        }

        if (!noRetType) // ctor or dtor for example doesn't have it
            out << toDStyle(m->getReturnType()) << " ";

        if (isOperator || m->getIdentifier() == nullptr)
            out << funcName;
        else
            out << m->getName().str();
        
        // runtime args
        out << "(";
        writeFnRuntimeArgs(m);
        out << ")";

        if (nogc) 
            out << " @nogc ";

        if (hasConstPtrToNonConst)
            out << "/* MANGLING OVERRIDE NOT YET FINISHED, ADJUST MANUALLY! */ ";
        
        // write function body
        if (m->isInlined() && m->hasInlineBody() && !isDtor)
        {
            auto writeMultilineExpr = [this, commentOut, ast](auto expr, bool ptrRet = false) {
                std::string s;
                llvm::raw_string_ostream os(s);
                std::stringstream ss;
                DPrinterHelper_PointerReturn rp;
                printPrettyD(expr, os, ptrRet ? &rp : nullptr, *DlangBindGenerator::g_printPolicy, 0, ast);
                ss << os.str();
                std::string line;
                for (int i = 0; std::getline(ss, line); i++)
                {
                    if (commentOut) 
                        out << "//";
                    textReplaceArrowColon(line);
                    out << line << std::endl;
                }
            };

            // for now just mix initializer list and ctor body in extra set of braces
            bool hasInitializerList = false;
            bool isEmptyBody = true;
            bool isTemplated = decl->isTemplated() || m->isTemplated();
            if (m->getBody())
            if (auto body = dyn_cast<CompoundStmt>(m->getBody())) // can it actually be anything else?
                isEmptyBody = body->body_empty();

            if (isCtor && !isTemplated)
            {
                const auto ctdecl = cast<CXXConstructorDecl>(m);
                hasInitializerList = ctdecl->getNumCtorInitializers() != 0
                    && std::find_if( ctdecl->init_begin(), ctdecl->init_end(), 
                        //[](auto x) {return x->isInClassMemberInitializer();}
                        [](auto x) {return x->isWritten();}
                        ) != ctdecl->init_end();

                if (hasInitializerList)
                    out << "{" << std::endl << "// initializer list" << std::endl;

                for(const auto init : ctdecl->inits())
                {
                    // This will be handled at some point later by memberIterate() 
                    if (init->isInClassMemberInitializer())
                        continue;
                    if (auto member = init->getMember())
                    {
                        //if (commentOut)
                        //    out << "//";
                        //out << sanitizedIdentifier(member->getNameAsString()) << " = ";
                        writeMultilineExpr(init);
                    }
                }

                if (hasInitializerList && !isEmptyBody)
                    out << "// ctor body" << std::endl;
            }

            if (!isEmptyBody)
            {
                // write body after initializer list
                writeMultilineExpr(m->getBody(), m->getReturnType()->isPointerType());
            }

            // close extra braces
            if (hasInitializerList && isCtor && !isTemplated)
            {
                if (commentOut)
                    out << "//";
                out << "}";
            }

            if (!m->getBody() || (isEmptyBody && !hasInitializerList))
            {
                out << ";";
            }
        }
        else
        {
            out << ";" << std::endl;
        }
    
        out << std::endl;
    }
}

void DlangBindGenerator::writeFnRuntimeArgs(const clang::FunctionDecl* fn)
{
    for (const auto fp : fn->parameters())
    {
        const auto typeStr = toDStyle(fp->getType());
        out << typeStr << " " << sanitizedIdentifier(fp->getName().str());

        if (const auto defaultVal = fp->getDefaultArg())
        {
            bool isNull = false;
            if (fp->getType()->isPointerType())
            {
                auto nullkind = defaultVal->isNullPointerConstant(fn->getASTContext(), Expr::NullPointerConstantValueDependence::NPC_NeverValueDependent);
                if (nullkind != Expr::NullPointerConstantKind::NPCK_NotNull)
                    isNull = true;
            }

            std::string s;
            llvm::raw_string_ostream os(s);
            if (isNull)
                os << "null";
            else 
                printPrettyD(defaultVal, os, nullptr, *DlangBindGenerator::g_printPolicy);
            out << " = " << os.str();

            // add rvalue-ref hack
            if (fp->getType()->isReferenceType()) 
            {
                // TODO: skip on function calls
                out << ".byRef ";
            }
        }

        if (fp != *(fn->param_end() - 1))
            out << ", ";
    }
}

void DlangBindGenerator::writeTemplateArgs(const clang::TemplateDecl* td)
{
    const auto tplist = td->getTemplateParameters();
    for (const auto tp : *tplist)
    {
        if (isa<NonTypeTemplateParmDecl>(tp))
        {
            auto nt = cast<NonTypeTemplateParmDecl>(tp);
            out << toDStyle(nt->getType()) << " ";
            if (auto defaultVal = nt->getDefaultArgument())
            {
                std::string s;
                llvm::raw_string_ostream os(s);
                printPrettyD(defaultVal, os, nullptr, *DlangBindGenerator::g_printPolicy);
                out << " = " << os.str();
            }
        }
        out << tp->getName().str();
        if (tp != *(tplist->end() - 1))
            out << ", ";
    }
}

void DlangBindGenerator::writeTemplateArgs(const clang::TemplateArgumentList* ta)
{
    bool first = true;
    for (const auto tp : ta->asArray())
    {
        auto tk = tp.getKind();
        std::string s;
        llvm::raw_string_ostream os(s);
        switch(tk)
        {
            case TemplateArgument::ArgKind::Integral:
                out << intTypeForSize(tp.getAsIntegral().getBitWidth()) << " T: ";
                out << tp.getAsIntegral().toString(10);
                break;
            case TemplateArgument::ArgKind::Expression:
                printPrettyD(tp.getAsExpr(), os, nullptr, *DlangBindGenerator::g_printPolicy);
                out << s;
                break;
            case TemplateArgument::ArgKind::Type:
                out << toDStyle(tp.getAsType());
                break;
            default: break;
        }
        if (first)
            first = false;
        else
            out << ", ";
    }
}

std::tuple<std::string, std::string> DlangBindGenerator::getFSPathPart(const std::string_view loc)
{
    std::string path;
    std::string lineCol;
    size_t colPos = std::string_view::npos;

    auto dotPos = loc.find_last_of('.');
    if (dotPos != std::string_view::npos)
    {
        colPos = loc.find_first_of(':', dotPos);
        if (colPos != std::string_view::npos)
        {
            lineCol = loc.substr(colPos);
            path = loc.substr(0, colPos);
        }
    }

    if (loc.length() && path.empty())
        path = std::string(loc);

    std::error_code _;
    path = fs::canonical(path, _).string();
    return std::make_tuple(path, lineCol);
}

std::tuple<std::string, std::string> DlangBindGenerator::getLineColumnPart(const std::string_view loc)
{
    std::string line;
    std::string col;

    auto colPos = loc.find_first_of(':');
    if (colPos != std::string_view::npos)
    {
        line = loc.substr(0, colPos-1);
        col = loc.substr(colPos+1);
    }
    else
        line = loc;

    return std::make_tuple(line, col);
}

std::string DlangBindGenerator::getNextMixinId()
{
    std::ostringstream ss;
    ss << "mxtid" << std::setw(3) << std::setfill('0') << mixinTemplateId;
    mixinTemplateId += 1;
    return ss.str();
}

std::tuple<std::string, std::string, bool> DlangBindGenerator::getOperatorName(const clang::FunctionDecl* decl)
{
    // get operator name and args
    const auto op = decl->getOverloadedOperator();
    const auto psize = decl->param_size();
    const bool isBinary = psize == 1;
    const bool isUnary = psize == 0;

    auto arityStr = [isBinary]() {return isBinary? "opBinary" : "opUnary";};
    auto getOpArgs = [](const std::string& s) { return std::string("string op : \"") + s + "\""; };

    bool customMangle = false;
    std::string funcName;
    std::string opSign;
    switch (op)
    {
    case OverloadedOperatorKind::OO_Plus:
        funcName = arityStr(); opSign = getOpArgs("+");
        break;
    case OverloadedOperatorKind::OO_Minus:
        funcName = arityStr(); opSign = getOpArgs("-");
        break;
    case OverloadedOperatorKind::OO_Star:
        funcName = arityStr(); opSign = getOpArgs("*");
        break;
    case OverloadedOperatorKind::OO_Slash:
        funcName = arityStr(); opSign = getOpArgs("/");
        break;
    case OverloadedOperatorKind::OO_Percent:
        funcName = arityStr(); opSign = getOpArgs("%");
        break;
    case OverloadedOperatorKind::OO_Caret:
        funcName = arityStr(); opSign = getOpArgs("^");
        break;
    case OverloadedOperatorKind::OO_Amp:
        funcName = arityStr(); opSign = getOpArgs("&");
        break;
    case OverloadedOperatorKind::OO_Pipe:
        funcName = arityStr(); opSign = getOpArgs("|");
        break;
    case OverloadedOperatorKind::OO_Tilde:
        funcName = arityStr(); opSign = getOpArgs("~");
        break;
    case OverloadedOperatorKind::OO_MinusMinus:
        funcName = arityStr(); opSign = getOpArgs("--");
        break;
    case OverloadedOperatorKind::OO_PlusPlus:
        funcName = arityStr(); opSign = getOpArgs("++");
        break;
    case OverloadedOperatorKind::OO_Call:
        funcName = "opCall";
        break;
    case OverloadedOperatorKind::OO_Subscript:
        funcName = "opIndex";
        break;
    case OverloadedOperatorKind::OO_AmpAmp:
        funcName = "op_and"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_PipePipe:
        funcName = "op_or"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_Less:
        funcName = "op_lt"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_Greater:
        funcName = "op_gt"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_LessEqual:
        funcName = "op_le"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_GreaterEqual:
        funcName = "op_ge"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_Exclaim:
        funcName = "op_not"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_ExclaimEqual:
        funcName = "op_ne"; customMangle = true;
        break;
    case OverloadedOperatorKind::OO_PlusEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("+");
        break;
    case OverloadedOperatorKind::OO_MinusEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("-");
        break;
    case OverloadedOperatorKind::OO_StarEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("*");
        break;
    case OverloadedOperatorKind::OO_SlashEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("/");
        break;
    case OverloadedOperatorKind::OO_PipeEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("|");
        break;
    case OverloadedOperatorKind::OO_AmpEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("&");
        break;
    case OverloadedOperatorKind::OO_CaretEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("^");
        break;
    case OverloadedOperatorKind::OO_LessLessEqual:
        funcName = "opOpAssign"; opSign = getOpArgs("<<");
        break;
    case OverloadedOperatorKind::OO_GreaterGreaterEqual:
        funcName = "opOpAssign"; opSign = getOpArgs(">>");
        break;
    case OverloadedOperatorKind::OO_Equal:
        funcName = "opAssign";
        break;
    case OverloadedOperatorKind::OO_EqualEqual:
        funcName = "opEquals";
        break;
    case OverloadedOperatorKind::OO_Arrow:
        funcName = "opUnary"; opSign = getOpArgs("->");
        break;
    default:
        funcName = "op"; opSign = getOperatorSpelling(op);
        break;
    }

    return std::make_tuple(funcName, opSign, customMangle);
}
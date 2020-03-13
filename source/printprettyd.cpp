#include "dlang_gen.h"

#include <sstream>
#include <iomanip>
#include <memory>

#include "llvm/Config/llvm-config.h"
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

#include "postedits.h"


using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::driver;
using namespace clang::tooling;




auto ret0 = returnStmt(has(integerLiteral(equals(0))));
bool DPrinterHelper_PointerReturn::handledStmt(clang::Stmt* E, llvm::raw_ostream& OS)
{
    return false;
}

namespace
{

std::string wrapRefHelper(QualType T, VarDecl* D)
{
    std::string s;
    llvm::raw_string_ostream os(s);
    // ret type
    os << DlangBindGenerator::toDStyle(T) << " ";
    // variable name
    os << DlangBindGenerator::sanitizedIdentifier(D->getName()) << "() { return ";
    // helper function body
    printPrettyD(D->getInit(), os, nullptr, *DlangBindGenerator::g_printPolicy);
    os << "; }";
    return os.str();
}

template<typename T>
class StmtFinderVisitor : public RecursiveASTVisitor<StmtFinderVisitor<T>>
{
public:
    T* node = nullptr;

    bool TraverseStmt(Stmt* stmt)
    {
        if (isa<T>(stmt))
        { 
            node = cast<T>(stmt);
            return false;
        }
        return true;
    }
};

// Looks for 'this' or '*this', when found aborts traversing
class HasCXXThisVisitor : public RecursiveASTVisitor<HasCXXThisVisitor>
{
public:
    CXXThisExpr* thisFound = nullptr;

    bool VisitUnaryOperator(UnaryOperator* Op)
    {
        if (Op->getOpcode() == UnaryOperator::Opcode::UO_Deref)
        {
            auto expr = Op->getSubExpr();
            if (expr && isa<CXXThisExpr>(expr))
            {
                thisFound = cast<CXXThisExpr>(expr);
                return false;
            }
        }
        return true;
    }

    bool VisitCXXThisExpr(CXXThisExpr* This)
    {
        thisFound = This;
        return false;
    }
};


class HasCXXTemporaryObjectExpr : public RecursiveASTVisitor<HasCXXTemporaryObjectExpr>
{
public:
    CXXTemporaryObjectExpr* found = nullptr;

    bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr *E)
    {
        found = E;
        return false;
    }
};

// NOTE: This class may contain code snippets from clang sources,
//  this may or may not impose some legal issues, but be careful.
class CppDASTPrinterVisitor : public RecursiveASTVisitor<CppDASTPrinterVisitor>
{
    raw_ostream &OS;
    unsigned IndentLevel;
    PrinterHelper* Helper;
    PrintingPolicy Policy;
    const ASTContext *Context;
    bool reverse = false;
    bool isCtorInitializer = false;
    FeedbackContext* Feedback;
public:
    explicit CppDASTPrinterVisitor(raw_ostream &os, PrinterHelper *helper,
                const PrintingPolicy &Policy, unsigned Indentation = 0,
                const ASTContext *Context = nullptr,
                FeedbackContext* Feedback = nullptr)
        : OS(os), IndentLevel(Indentation), Helper(helper), Policy(Policy),
          Context(Context), Feedback(Feedback) {}

    raw_ostream &Indent(int Delta = 0) {
      for (int i = 0, e = IndentLevel+Delta; i < e; ++i)
        OS << "  ";
      return OS;
    }

    bool shouldTraversePostOrder() const { return reverse; }

    bool VisitVarDecl(VarDecl *D)
    {
        //prettyPrintPragmas(D);

        QualType T = D->getTypeSourceInfo()
                         ? D->getTypeSourceInfo()->getType()
                         : D->getASTContext().getUnqualifiedObjCPointerType(D->getType());

        /*
        if (!Policy.SuppressSpecifiers)
        {
            StorageClass SC = D->getStorageClass();
            if (SC != SC_None)
                Out << VarDecl::getStorageClassSpecifierString(SC) << " ";

            if (D->isModulePrivate())
                Out << "private ";

            if (D->isConstexpr())
            {
                Out << "immutable ";
                T.removeLocalConst();
            }
        }
        */

       // Isn't this handled by toDStyle() ?
       if  ( D->getStorageClass() == StorageClass::SC_Static )
            OS << "__gshared static ";

        bool isRef = T->isReferenceType() && D->getInit();
        if (isRef)
        {
            OS << wrapRefHelper(T,D);
            return false;
        }
        //printDeclType(T, D->getName());
        auto typeString = DlangBindGenerator::toDStyle(T);
        OS << typeString << " " << DlangBindGenerator::sanitizedIdentifier(D->getName());
        Expr *Init = D->getInit();
        if (!Policy.SuppressInitializers && Init)
        {
            bool ImplicitInit = false;
            if (CXXConstructExpr *Construct =
                    dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit()))
            {
                if (D->getInitStyle() == VarDecl::CallInit &&
                    !Construct->isListInitialization())
                {
                    ImplicitInit = Construct->getNumArgs() == 0 ||
                                   Construct->getArg(0)->isDefaultArgument();
                }
            }
            if (!ImplicitInit)
            {
                if ((D->getInitStyle() == VarDecl::CallInit))
                    OS << " = " << typeString << "("; // D doesn't have C++ ctor call syntax for variables
                else if (D->getInitStyle() == VarDecl::CInit)
                {
                    OS << " = ";
                }

                TraverseStmt(Init);
                if ((D->getInitStyle() == VarDecl::CallInit))
                    OS << ")";
            }
        }
        //prettyPrintAttributes(D);
        return false;
    }

    void printRawDeclStmt(DeclStmt* Node)
    {
        const auto len = std::distance(Node->decl_begin(), Node->decl_end());
        auto* last = *(Node->decl_end() - 1);
        for(auto* d : Node->decls())
        {
            TraverseDecl(d);
            if (len > 1 && d != last)
                OS << "; ";
        }
    }

    bool VisitDeclStmt(DeclStmt* Node)
    {
        printRawDeclStmt(Node);
        OS << ";";
        return false;
    }

    bool VisitNullStmt(NullStmt* Null)
    {
        //Indent() << "{}";
        OS << "{}";
        return true;
    }

    bool VisitCStyleCastExpr(CStyleCastExpr *E)
    {
        if (E->getCastKind() == CastKind::CK_ToVoid && HandlePotentialAssert(E))
        {
            return false;
        }
        else
        {
            //OS << "cast(" << E->getTypeAsWritten().getAsString() << ")";
            OS << "cast(" << DlangBindGenerator::toDStyle(E->getTypeAsWritten()) << ")";
        }
        return true;
    }

    bool VisitCompoundStmt(CompoundStmt *Node)
    {
        OS << "{\n";
        IndentLevel += 2;

        
        for (auto *I : Node->body())
        {
            //Indent();
            TraverseStmt(I);
            if (I && isa<Expr>(I))
                OS << ";"; // normally after Expr's only
            OS << '\n';
        }

        IndentLevel -= 2;
        //Indent();
        OS << "}";
        return false;
    }

    bool VisitReturnStmt(ReturnStmt *Node) 
    {
        OS << "return";
        if (Node->getRetValue()) {
            OS << " ";
            TraverseStmt(Node->getRetValue());
        }
        OS << ";";
        //if (Policy.IncludeNewlines) OS << "\n";
        return false;
    }

    bool VisitLabelStmt(LabelStmt *Node) 
    {
        OS << Node->getName() << ":\n";
        //TraverseStmt(Node->getSubStmt());
        return true;
    }

    bool VisitGotoStmt(GotoStmt *Node) 
    {
        OS << "goto " << Node->getLabel()->getName() << ";\n";
        return true;
    }

    bool VisitContinueStmt(ContinueStmt *Node) 
    {
        OS << "continue;\n";
        return true;
    }

    bool VisitBreakStmt(BreakStmt *Node) 
    {
        OS << "break;\n";
        return true;
    }

    bool VisitIfStmt(IfStmt *If)
    {
        //Indent();
        OS << "if (";
        if (const DeclStmt *DS = If->getConditionVariableDeclStmt())
            TraverseDeclStmt(const_cast<DeclStmt*>(DS));
        else
            TraverseStmt(If->getCond());
        OS << ')';

        if (auto *CS = dyn_cast<CompoundStmt>(If->getThen()))
        {
            OS << ' ';
            VisitCompoundStmt(CS);
            OS << (If->getElse() ? ' ' : '\n');
        }
        else
        {
            OS << '\n';
            TraverseStmt(If->getThen());
            if (isa<Expr>(If->getThen()))
                OS << ";\n";
            //if (If->getElse())
                //Indent();
        }

        if (Stmt *Else = If->getElse())
        {
            OS << "else";

            if (auto *CS = dyn_cast<CompoundStmt>(Else))
            {
                OS << ' ';
                VisitCompoundStmt(CS);
                OS << '\n';
            }
            else if (auto *ElseIf = dyn_cast<IfStmt>(Else))
            {
                OS << ' ';
                VisitIfStmt(ElseIf);
            }
            else
            {
                OS << '\n';
                TraverseStmt(If->getElse());
                if (isa<Expr>(If->getElse()))
                    OS << ";\n";
            }
        }
        return false;
    }

    bool VisitCaseStmt(CaseStmt *Node) 
    {
        OS << "case ";
        TraverseStmt(Node->getLHS());
        // GNU specific extension, nothing to look at, just move on
        #if 0
        if (Node->getRHS())
        {
            OS << " ... ";
            TraverseStmt(Node->getRHS());
        }
        #endif
        OS << ":\n";
        TraverseStmt(Node->getSubStmt());
        if (isa<Expr>(Node->getSubStmt()))
            OS << ";";
        return false;
    }

    bool VisitSwitchStmt(SwitchStmt *Node)
    {
        //Indent();
        OS << "switch (";
        if (const DeclStmt *DS = Node->getConditionVariableDeclStmt())
            VisitDeclStmt(const_cast<DeclStmt*>(DS));
        else
            TraverseStmt(Node->getCond());
        OS << ")";

        // Pretty print compoundstmt bodies (very common).
        if (auto *CS = dyn_cast<CompoundStmt>(Node->getBody()))
        {
            OS << " ";
            VisitCompoundStmt(CS);
            OS << "\n";
        }
        else
        {
            OS << "\n";
            TraverseStmt(Node->getBody());
        }
        return false;
    }

    bool VisitWhileStmt(WhileStmt *Node)
    {
        //Indent();
        OS << "while (";
        if (const DeclStmt *DS = Node->getConditionVariableDeclStmt())
            VisitDeclStmt(const_cast<DeclStmt*>(DS));
        else
            TraverseStmt(Node->getCond());
        OS << ")\n";
        TraverseStmt(Node->getBody());
        return false;
    }

    bool VisitDoStmt(DoStmt *Node)
    {
        //Indent();
        OS << "do ";
        if (auto *CS = dyn_cast<CompoundStmt>(Node->getBody()))
        {
            VisitCompoundStmt(const_cast<CompoundStmt*>(CS));
            OS << " ";
        }
        else
        {
            OS << "\n";
            TraverseStmt(Node->getBody());
            //Indent();
        }

        OS << "while (";
        TraverseStmt(Node->getCond());
        OS << ");\n";
        return false;
    }

    bool VisitForStmt(ForStmt *Node)
    {
        //Indent();
        OS << "for (";
        if (Node->getInit())
        {
            if (auto *DS = dyn_cast<DeclStmt>(Node->getInit()))
                printRawDeclStmt(const_cast<DeclStmt*>(DS));
            else
                TraverseStmt(cast<Expr>(Node->getInit()));
        }
        OS << ";";
        if (Node->getCond())
        {
            OS << " ";
            TraverseStmt(Node->getCond());
        }
        OS << ";";
        if (Node->getInc())
        {
            OS << " ";
            TraverseStmt(Node->getInc());
        }
        OS << ") ";

        if (auto *CS = dyn_cast<CompoundStmt>(Node->getBody()))
        {
            VisitCompoundStmt(CS);
            OS << "\n";
        }
        else
        {
            OS << "\n";
            TraverseStmt(Node->getBody());
            OS << ";";
        }
        return false;
    }

    bool VisitArraySubscriptExpr(ArraySubscriptExpr *Node) {
        auto arrExpr = Node->getLHS();

        // selectively avoid adding .ptr to array index access
        // prevents issues like 'arr.ptr[0] = 0' where it should be 'arr[0] = 0'
        auto impcast = dyn_cast<ImplicitCastExpr>(Node->getLHS());
        if (impcast && impcast->getCastKind() == CastKind::CK_ArrayToPointerDecay)
            arrExpr = arrExpr->IgnoreImpCasts();
        
        TraverseStmt(arrExpr);
        OS << "[";
        TraverseStmt(Node->getRHS());
        OS << "]";
        return false;
    }

    bool TraverseConstructorInitializer(CXXCtorInitializer *I)
    {
        isCtorInitializer = true;
        if (!I->isWritten())
            return false;

        auto m = I->getMember();
        auto builtin = m->getType()->isBuiltinType() || m->getType()->isAnyPointerType();
        OS << DlangBindGenerator::sanitizedIdentifier(m->getNameAsString()) << " = ";
        //if (!builtin)
        //    OS << DlangBindGenerator::toDStyle(m->getType()) << "(";

        TraverseStmt(I->getInit());

        //if (!builtin)
        //    OS << ")";
        OS << ";\n";

        return false;
    }

    bool VisitCXXConstructExpr(CXXConstructExpr *E)
    {
        auto br = E->getParenOrBraceRange();
        bool canBeImplicit = E->getConstructor()->isImplicitlyInstantiable();
        bool prependType = isCtorInitializer;

        if(E->isListInitialization())
            prependType = true;

        StmtFinderVisitor<CXXTemporaryObjectExpr> finder;
        finder.TraverseStmt(E);
        if (finder.node)
            prependType = true;
        
        // hacky way
        if (E->getNumArgs() == 1 && isa<Expr>(E->getArg(0)))
            prependType = false;

        // if doesn't have braces might mean it is implicit ctor match
        if (prependType)// && !isCtorInitializer)
        {
            OS << DlangBindGenerator::toDStyle(E->getType());
        }

        if ((prependType) || (E->isListInitialization() && !E->isStdInitListInitialization()))
            OS << "(";

        for (unsigned i = 0, e = E->getNumArgs(); i != e; ++i)
        {
            if (isa<CXXDefaultArgExpr>(E->getArg(i)))
            {
                // Don't print any defaulted arguments
                break;
            }

            if (i)
                OS << ", ";
            TraverseStmt(E->getArg(i));
        }

        if ( (prependType) || (E->isListInitialization() && !E->isStdInitListInitialization()))
            OS << ")";

        return false;
    }

    static bool isImplicitThis(const Expr *E) {
        if (const auto *TE = dyn_cast<CXXThisExpr>(E))
            return TE->isImplicit();
        return false;
    }

    bool VisitImplicitCastExpr(ImplicitCastExpr *Node) {
        // Be careful!
        // Can't use dynamic cast here because RTTI is disabled for clang on Linux
        if (Context && Helper && static_cast<DPrinterHelper_PointerReturn*>(Helper))
        {
            // This needs to be handled somehow, forcing dependent types to be not null is not right
            auto kind = Node->isNullPointerConstant(*const_cast<ASTContext*>(Context), Expr::NullPointerConstantValueDependence::NPC_ValueDependentIsNotNull);
            if (kind != Expr::NullPointerConstantKind::NPCK_NotNull)
            {
                OS << "null";
                return false;
            }
        }
        // add .ptr suffix for all array casts except string literals
        if (Node->getCastKind() == CastKind::CK_ArrayToPointerDecay)
        {
            TraverseStmt(Node->getSubExpr());
            if (!isa<StringLiteral>(Node->IgnoreImpCasts()))
                OS << ".ptr";
            return false;
        }
        if (Node->getCastKind() == CastKind::CK_NullToPointer)
        {
            OS << "null";
            return false;
        }
        return true;
    }

    bool VisitMemberExpr(MemberExpr *Node)
    {
        ImplicitCastExpr* ic = nullptr;
        bool thisBase = (Node->getBase() && Node->getBase()->getStmtClass() == Stmt::CXXThisExprClass);
        if (!Policy.SuppressImplicitBase || !isImplicitThis(Node->getBase()))
        {
            TraverseStmt(Node->getBase());

            auto *ParentMember = dyn_cast<MemberExpr>(Node->getBase());
            FieldDecl *ParentDecl =
                ParentMember ? dyn_cast<FieldDecl>(ParentMember->getMemberDecl())
                             : nullptr;

            if (!ParentDecl || !ParentDecl->isAnonymousStructOrUnion())
            {
                // Ignore non-polymorphic bases for D (avoids empty leading dot in some cases)
                bool implcast = isa<ImplicitCastExpr>(Node->getBase());
                bool ignoreBase = false;
                
                if (implcast)
                    ic = cast<ImplicitCastExpr>(Node->getBase());
                // Check for implicit base cast
                if (ic)
                {
                    if (ic->isImplicitCXXThis())
                        ignoreBase = true;
                    else if (auto cls = ic->getBestDynamicClassType(); ic->isImplicitCXXThis() && cls)
                        if (!hasVirtualMethods(cls))
                            ignoreBase = true;
                }

                if (!ignoreBase)
                    OS << ".";
            }
        }

        if (auto *FD = dyn_cast<FieldDecl>(Node->getMemberDecl()))
            if (FD->isAnonymousStructOrUnion())
                return false;

        if (NestedNameSpecifier *Qualifier = Node->getQualifier()) {
            if (ic && ic->getBestDynamicClassType() && Qualifier->getKind() == NestedNameSpecifier::SpecifierKind::TypeSpec)
            {
                if (auto rec = Qualifier->getAsRecordDecl())
                    if (ic->getBestDynamicClassType()->isDerivedFrom(rec))
                        {} // intentionally left blank
                    else 
                        Qualifier->print(OS, Policy);
            }
            else
                Qualifier->print(OS, Policy);
        }
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";

        // Write hand written operator calls
        if (auto methodDecl = dyn_cast<CXXMethodDecl>(Node->getMemberDecl()); methodDecl && methodDecl->isOverloadedOperator())
        {
            auto [name, opSign, _] = DlangBindGenerator::getOperatorName(methodDecl);
            OS << name;
            if (!opSign.empty()) 
                OS << "!(" << opSign << ")";
        }
        else
        {
            OS <<  DlangBindGenerator::sanitizedIdentifier(
                Node->getMemberNameInfo().getName().getAsString());
        }
        // TODO: template args
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);

        return false;
    }

    bool VisitParenExpr(ParenExpr *Node) 
    {
        OS << "(";
        TraverseStmt(Node->getSubExpr());
        OS << ")";
        return false;
    }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *Node) 
    {
        const char *OpStrings[NUM_OVERLOADED_OPERATORS] = {
            "",
        #define OVERLOADED_OPERATOR(Name,Spelling,Token,Unary,Binary,MemberOnly) \
            Spelling,
        #include "clang/Basic/OperatorKinds.def"
        };

        OverloadedOperatorKind Kind = Node->getOperator();
        if (Kind == OO_PlusPlus || Kind == OO_MinusMinus) {
            if (Node->getNumArgs() == 1) {
            OS << OpStrings[Kind] << ' ';
            TraverseStmt(Node->getArg(0));
            } else {
            TraverseStmt(Node->getArg(0));
            OS << ' ' << OpStrings[Kind];
            }
        } else if (Kind == OO_Arrow) {
            TraverseStmt(Node->getArg(0));
        } else if (Kind == OO_Call) {
            TraverseStmt(Node->getArg(0));
            OS << '(';
            for (unsigned ArgIdx = 1; ArgIdx < Node->getNumArgs(); ++ArgIdx) {
            if (ArgIdx > 1)
                OS << ", ";
            if (!isa<CXXDefaultArgExpr>(Node->getArg(ArgIdx)))
                TraverseStmt(Node->getArg(ArgIdx));
            }
            OS << ')';
        } else if (Kind == OO_Subscript) {
            TraverseStmt(Node->getArg(0));
            OS << '[';
            TraverseStmt(Node->getArg(1));
            OS << ']';
        } else if (Node->getNumArgs() == 1) {
            OS << OpStrings[Kind] << ' ';
            TraverseStmt(Node->getArg(0));
        } else if (Node->getNumArgs() == 2) {
            TraverseStmt(Node->getArg(0));
            OS << ' ' << OpStrings[Kind] << ' ';
            TraverseStmt(Node->getArg(1));
        } else {
            llvm_unreachable("unknown overloaded operator");
        }
        return false;
    }

    void PrintCallArgs(CallExpr *Call) 
    {
        for (unsigned i = 0, e = Call->getNumArgs(); i != e; ++i) 
        {
            if (isa<CXXDefaultArgExpr>(Call->getArg(i))) {
                // Don't print any defaulted arguments
                break;
            }

            if (i) { OS << ", "; }
            TraverseStmt(Call->getArg(i));

            // add .byRef hack for temporary objects
            // HACK: this fails on variadic functions/ptr to functions in some cases, for now this second if just prevents crash
            if (auto fn = Call->getDirectCallee(); fn && fn->getNumParams() && !fn->isVariadic())
            {
                auto fp = fn->getParamDecl(i);
                if (fp->getType()->isReferenceType() && isa<MaterializeTemporaryExpr>(Call->getArg(i)))
                {
                    OS << ".byRef";
                    if (auto recordDecl = Call->getArg(i)->getType()->getAsRecordDecl())
                    {
                    Feedback->addAction(
                        std::move(std::make_unique<AddRvalueHackAction>(recordDecl->getName()))
                        );
                    }
                }
            }
        }
    }


    void PrintCallExpr(CallExpr *Call)
    {
        TraverseStmt(Call->getCallee());

        // dont add parens after destroy()
        if (isa<CXXPseudoDestructorExpr>(Call->getCallee()))
            return;

        OS << "(";
        PrintCallArgs(Call);
        OS << ")";
    }

    bool VisitCallExpr(CallExpr *Call) 
    {
        if (isa<CXXOperatorCallExpr>(Call))
            return true;

        if (isa<CXXMemberCallExpr>(Call))
            return true;

        PrintCallExpr(Call);
        return false;
    }

    bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Node) 
    {
        // If we have a conversion operator call only print the argument.
        CXXMethodDecl *MD = Node->getMethodDecl();
        if (MD && isa<CXXConversionDecl>(MD)) {
            TraverseStmt(Node->getImplicitObjectArgument());
            return false;
        }
        PrintCallExpr(Node);
        return false;
    }

    bool VisitDeclRefExpr(DeclRefExpr *Node)
    {
        if (const auto *OCED = dyn_cast<OMPCapturedExprDecl>(Node->getDecl()))
        {
            OCED->getInit()->IgnoreImpCasts()->printPretty(OS, nullptr, Policy);
            return true;
        }
        if (NestedNameSpecifier *Qualifier = Node->getQualifier()) 
        {
            switch(Qualifier->getKind())
            {
                case NestedNameSpecifier::SpecifierKind::TypeSpec:
                case NestedNameSpecifier::SpecifierKind::TypeSpecWithTemplate:
                    OS << DlangBindGenerator::toDStyle(QualType(Qualifier->getAsType(), 0)) << ".";
                    break;
            }
            //else
            //    Qualifier->print(OS, Policy);
        }
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";
        OS <<  DlangBindGenerator::sanitizedIdentifier(
                Node->getNameInfo().getAsString());
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);
        return true;
    }

    bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr *Node) 
    {
        if (NestedNameSpecifier *Qualifier = Node->getQualifier())
            switch(Qualifier->getKind())
            {
                case NestedNameSpecifier::SpecifierKind::TypeSpec:
                case NestedNameSpecifier::SpecifierKind::TypeSpecWithTemplate:
                    OS << DlangBindGenerator::toDStyle(QualType(Qualifier->getAsType(), 0)) << ".";
                    break;
                default:
                    Qualifier->print(OS, Policy);
            }
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";
        OS << Node->getNameInfo();
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);
        return true;
    }

    bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *Node)
    {
        bool typeofThis = false;
        if (Node->isArgumentType()) {
            OS << '(';
            OS << DlangBindGenerator::toDStyle(Node->getArgumentType());
            OS << ')';
        } else {
            OS << " ";
            HasCXXThisVisitor finder;
            finder.TraverseStmt(Node->getArgumentExpr());
            if (!finder.thisFound || finder.thisFound->isImplicitCXXThis())
                TraverseStmt(Node->getArgumentExpr());
            else
            {
                typeofThis = true;
                OS << "typeof(this)";
            }
        }

        switch(Node->getKind()) {
        case UETT_SizeOf:
            OS << ".sizeof"; return false;
            break;
        case UETT_AlignOf:
            OS << ".alignof"; return false;
            break;
        }
        
        return !typeofThis;
    }

    bool VisitCXXNewExpr(CXXNewExpr *E)
    {
        //if (E->isGlobalNew())
        //    OS << "::";
        unsigned NumPlace = E->getNumPlacementArgs();
        if (NumPlace > 0 && !isa<CXXDefaultArgExpr>(E->getPlacementArg(0)))
        {
            OS << "emplace( ";
            // WTH is this happening??
            if (E->getPlacementArg(0)->getStmtClass() == Stmt::CXXTemporaryObjectExprClass)
                VisitCXXTemporaryObjectExpr(cast<CXXTemporaryObjectExpr>(E->getPlacementArg(0)));
            else
                TraverseStmt(E->getPlacementArg(0));
            for (unsigned i = 1; i < NumPlace; ++i)
            {
                if (isa<CXXDefaultArgExpr>(E->getPlacementArg(i)))
                    break;
                OS << ", ";
                TraverseStmt(E->getPlacementArg(i));
            }
            OS << ", ";
            //OS << ") ";
        }
        else OS << "new ";
        /*
        std::string TypeS;
        if (Expr *Size = E->getArraySize())
        {
            llvm::raw_string_ostream s(TypeS);
            s << '[';
            Size->printPretty(s, Helper, Policy);
            s << ']';
        }
        E->getAllocatedType().print(OS, Policy, TypeS);
        */
        OS << DlangBindGenerator::toDStyle(E->getAllocatedType());

        CXXNewExpr::InitializationStyle InitStyle = E->getInitializationStyle();
        if (InitStyle)
        {
            if (InitStyle == CXXNewExpr::CallInit)
                OS << "(";
            TraverseStmt(E->getInitializer());
            if (InitStyle == CXXNewExpr::CallInit)
                OS << ")";
        }

        if (NumPlace > 0)
            OS << ")";

        return false;
    }

    bool VisitCXXDeleteExpr(CXXDeleteExpr *E)
    {
        OS << "destroy(";
        TraverseStmt(E->getArgument());
        OS << ");\n";
        return false;
    }

    bool VisitCXXPseudoDestructorExpr(CXXPseudoDestructorExpr *E)
    {
        OS << "destroy(";
        TraverseStmt(E->getBase());
        OS << ")";
        return false;
    }

    bool VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *Node)
    {
        return true;
    }

    bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr *Node)
    {
        if (!Node->isImplicitAccess())
        {
            TraverseStmt(Node->getBase());
            OS << ".";
        }
        //if (NestedNameSpecifier *Qualifier = Node->getQualifier())
        //    Qualifier->print(OS, Policy);
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";
        OS << Node->getMemberNameInfo();
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);
        return Node->isImplicitAccess();
    }

    bool VisitCXXUnresolvedConstructExpr(CXXUnresolvedConstructExpr *Node) {
        OS << DlangBindGenerator::toDStyle(Node->getTypeAsWritten());
        OS << "(";
        for(auto Arg = Node->arg_begin(); Arg != Node->arg_end(); Arg++) 
        {
            if (Arg != Node->arg_begin())
                OS << ", ";
            TraverseStmt(*Arg);
        }
        OS << ")";
        return false;
    }

    bool VisitUnresolvedMemberExpr(UnresolvedMemberExpr *Node) 
    {
        if (!Node->isImplicitAccess()) {
            TraverseStmt(Node->getBase());
            OS << ".";
        }
        if (NestedNameSpecifier *Qualifier = Node->getQualifier())
            Qualifier->print(OS, Policy);
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";
        OS << DlangBindGenerator::sanitizedIdentifier(Node->getMemberNameInfo().getAsString());
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);
        return true;
    }

    bool VisitUnresolvedLookupExpr(UnresolvedLookupExpr *Node) 
    {
        if (Node->getQualifier())
            Node->getQualifier()->print(OS, Policy);
        //if (Node->hasTemplateKeyword())
        //    OS << "template ";
        OS << DlangBindGenerator::sanitizedIdentifier(Node->getNameInfo().getAsString());
        if (Node->hasExplicitTemplateArgs())
            printDTemplateArgumentList(OS, Node->template_arguments(), Policy);
        return true;
    }

    bool VisitCXXNamedCastExpr(CXXNamedCastExpr *Node) 
    {
        bool isRef = Node->getTypeAsWritten()->isReferenceType();
        OS << "cast(";
        if (isRef)
            OS << DlangBindGenerator::toDStyle(Node->getTypeAsWritten()->getPointeeType());
        else
            OS << DlangBindGenerator::toDStyle(Node->getTypeAsWritten());
        OS << ")(";
        TraverseStmt(Node->getSubExpr());
        OS << ")";
        return false;
    }

    bool VisitCXXStaticCastExpr(CXXStaticCastExpr *Node) 
    {
        VisitCXXNamedCastExpr(Node);
        return false;
    }

    bool VisitCXXDynamicCastExpr(CXXDynamicCastExpr *Node) 
    {
        VisitCXXNamedCastExpr(Node);
        return false;
    }

    bool VisitCXXReinterpretCastExpr(CXXReinterpretCastExpr *Node) 
    {
        VisitCXXNamedCastExpr(Node);
        return false;
    }

    bool VisitCXXConstCastExpr(CXXConstCastExpr *Node) 
    {
        VisitCXXNamedCastExpr(Node);
        return false;
    }

    bool VisitCXXFunctionalCastExpr(CXXFunctionalCastExpr *Node) 
    {
        OS << DlangBindGenerator::toDStyle(Node->getType());
        // If there are no parens, this is list-initialization, and the braces are
        // part of the syntax of the inner construct.
        if (Node->getLParenLoc().isValid())
            OS << "(";
        TraverseStmt(Node->getSubExpr());
        if (Node->getLParenLoc().isValid())
            OS << ")";
        return false;
    }

    bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr *Node) 
    {
        //Node->getType().print(OS, Policy);
        OS << DlangBindGenerator::toDStyle(Node->getType());
        if (Node->isStdInitListInitialization())
        {
            /* Nothing to do; braces are part of creating the std::initializer_list. */;
        }
        else if (Node->isListInitialization())
            OS << "{";
        else
            OS << "(";
        for (CXXTemporaryObjectExpr::arg_iterator Arg = Node->arg_begin(),
                                                ArgEnd = Node->arg_end();
            Arg != ArgEnd; ++Arg) {
            if ((*Arg)->isDefaultArgument())
            break;
            if (Arg != Node->arg_begin())
            OS << ", ";
            TraverseStmt(*Arg);
        }
        if (Node->isStdInitListInitialization())
        {
            /* See above. */;
        }
        else if (Node->isListInitialization())
            OS << "}";
        else
            OS << ")";
        return false;
    }

    bool VisitConditionalOperator(ConditionalOperator *Node) 
    {
        TraverseStmt(Node->getCond());
        OS << " ? ";
        TraverseStmt(Node->getLHS());
        OS << " : ";
        TraverseStmt(Node->getRHS());
        return false;
    }

    bool VisitBinaryOperator(BinaryOperator *Node) 
    {
        bool isPtr = false;
        if (auto member = dyn_cast<MemberExpr>(Node->getLHS()))
            isPtr = member->getType()->isPointerType();
        else if (auto declref = dyn_cast<DeclRefExpr>(Node->getLHS()))
            isPtr = declref->getType()->isPointerType();

        // outputs LHS 'op' RHS
        TraverseStmt(Node->getLHS());
        OS << " " << BinaryOperator::getOpcodeStr(Node->getOpcode()) << " ";

        auto noImpCastsRHS = Node->getRHS()->IgnoreImpCasts();
        if (isPtr && isa<IntegerLiteral>(noImpCastsRHS))
        {
            bool isNullVal;
#if (LLVM_VERSION_MAJOR < 8)
            llvm::APSInt res;
            isNullVal = noImpCastsRHS->EvaluateAsInt(res, *Context) && res.isNullValue();
#else
            Expr::EvalResult res;
            isNullVal = noImpCastsRHS->EvaluateAsInt(res, *Context) && res.Val.getInt().isNullValue();
#endif
            if (isNullVal)
            {
                OS << "null";
            }
            else
            {
                OS << "cast(" << DlangBindGenerator::toDStyle(Node->getLHS()->getType()) << ") ";
                TraverseStmt(Node->getRHS());
            }
        }
        else
        {
            TraverseStmt(Node->getRHS());
        }

        return false;
    }

    bool VisitUnaryOperator(UnaryOperator *Node) {
        if (!Node->isPostfix()) {
            if (!isa<CXXThisExpr>(Node->getSubExpr()))
                OS << UnaryOperator::getOpcodeStr(Node->getOpcode());

            // Print a space if this is an "identifier operator" like __real, or if
            // it might be concatenated incorrectly like '+'.
            switch (Node->getOpcode()) {
            default: break;
            case UO_Real:
            case UO_Imag:
            case UO_Extension:
            OS << ' ';
            break;
            case UO_Plus:
            case UO_Minus:
            if (isa<UnaryOperator>(Node->getSubExpr()))
                OS << ' ';
            break;
            }
        }
        TraverseStmt(Node->getSubExpr());

        if (Node->isPostfix())
            OS << UnaryOperator::getOpcodeStr(Node->getOpcode());

        return false;
    }

    bool VisitCXXBoolLiteralExpr(CXXBoolLiteralExpr *Node) {
        OS << (Node->getValue() ? "true" : "false");
        return true;
    }

    bool VisitCXXNullPtrLiteralExpr(CXXNullPtrLiteralExpr *Node) {
        OS << "null";
        return true;
    }
    bool VisitCXXThisExpr(CXXThisExpr *Node) {
        if (!(Node->isImplicit() || Node->isImplicitCXXThis()))
            OS << "this";
        return true;
    }

    bool VisitFloatingLiteral(FloatingLiteral *Node) 
    {
        SmallString<16> Str;
        Node->getValue().toString(Str);
        OS << Str;
        switch (Node->getType()->getAs<BuiltinType>()->getKind()) {
            default: llvm_unreachable("Unexpected type for float literal!");
            case BuiltinType::Half:       break; // FIXME: suffix?
            case BuiltinType::Double:     break; // no suffix.
            case BuiltinType::Float16:    break;
            case BuiltinType::Float:      OS << 'f'; break;
            case BuiltinType::LongDouble: break;
            case BuiltinType::Float128:   break;
        }
        return true;
    }

    bool VisitIntegerLiteral(IntegerLiteral *Node) 
    {
        /*
        if (Policy.ConstantsAsWritten && printExprAsWritten(OS, Node, Context))
            return;
        */
        bool isSigned = Node->getType()->isSignedIntegerType();
        OS << Node->getValue().toString(10, isSigned);

        // Emit suffixes.  Integer literals are always a builtin integer type.
        switch (Node->getType()->getAs<BuiltinType>()->getKind()) 
        {
            default: llvm_unreachable("Unexpected type for integer literal!");
            case BuiltinType::Char_S:
            case BuiltinType::Char_U:    break;
            case BuiltinType::UChar:     break;
            case BuiltinType::Short:     break;
            case BuiltinType::UShort:    break;
            case BuiltinType::Int:       break; // no suffix.
            case BuiltinType::UInt:      OS << 'U'; break;
            case BuiltinType::Long:      OS << 'L'; break;
            case BuiltinType::ULong:     OS << "UL"; break;
            case BuiltinType::LongLong:  OS << "L"; break;
            case BuiltinType::ULongLong: OS << "UL"; break;
        }
        return true;
    }

    bool VisitStringLiteral(StringLiteral *Str) {
        // wide strings have 'L' prefix, strip it
        if (Str->isWide())
        {
            std::string buf;
            llvm::raw_string_ostream temp(buf);
            Str->outputString(temp);
            temp.flush();
            OS << buf.substr(buf[0] == 'L' ? 1 : 0);
        }
        else
            Str->outputString(OS);

        return true;
    }

    bool VisitCharacterLiteral(CharacterLiteral *Node)
    {
        unsigned value = Node->getValue();

        switch (value)
        {
        case '\\':
            OS << "'\\\\'";
            break;
        case '\'':
            OS << "'\\''";
            break;
        case '\a':
            // TODO: K&R: the meaning of '\\a' is different in traditional C
            OS << "'\\a'";
            break;
        case '\b':
            OS << "'\\b'";
            break;
        // Nonstandard escape sequence.
        /*case '\e':
            OS << "'\\e'";
            break;*/
        case '\f':
            OS << "'\\f'";
            break;
        case '\n':
            OS << "'\\n'";
            break;
        case '\r':
            OS << "'\\r'";
            break;
        case '\t':
            OS << "'\\t'";
            break;
        case '\v':
            OS << "'\\v'";
            break;
        default:
            // A character literal might be sign-extended, which
            // would result in an invalid \U escape sequence.
            // FIXME: multicharacter literals such as '\xFF\xFF\xFF\xFF'
            // are not correctly handled.
            if ((value & ~0xFFu) == ~0xFFu && Node->getKind() == CharacterLiteral::Ascii)
                value &= 0xFFu;
            if (value < 256 && isPrintable((unsigned char)value))
                OS << "'" << (char)value << "'";
            else if (value < 256)
                OS << "'\\x" << llvm::format("%02x", value) << "'";
            else if (value <= 0xFFFF)
                OS << "'\\u" << llvm::format("%04x", value) << "'";
            else
                OS << "'\\U" << llvm::format("%08x", value) << "'";
        }

        switch (Node->getKind())
        {
        case CharacterLiteral::Ascii:
            break; // no prefix.
        case CharacterLiteral::Wide:
            OS << 'w';
            break;
        case CharacterLiteral::UTF8:
            break;
        case CharacterLiteral::UTF16:
            OS << 'w';
            break;
        case CharacterLiteral::UTF32:
            OS << 'd';
            break;
        }
        return true;
    }

    bool HandlePotentialAssert(CStyleCastExpr* E)
    {
        // Detect if this cast expands to parens containing logical op with right hand being comma expr containing assert function
        // (void)((!!(expr)) || _wassert(msg, file, line), 0)
        if (auto outerParens = dyn_cast<ParenExpr>(E->getSubExpr()))
        {
            if (auto logicOp = dyn_cast<BinaryOperator>(outerParens->getSubExpr()))
            {
                auto commaExpr = dyn_cast<BinaryOperator>(logicOp->getRHS()->IgnoreImpCasts()->IgnoreParens());
                if (commaExpr && commaExpr->getOpcode() == BinaryOperatorKind::BO_Comma)
                {
                    if (auto callExpr = dyn_cast<CallExpr>(commaExpr->getLHS()))
                    {
                        auto fnref = callExpr->getDirectCallee();
                        if (fnref && fnref->getNameInfo().getAsString().find("assert") != std::string::npos)
                        {
                            // convert to normal assert
                            if (callExpr->getNumArgs() == 3 
                                && (logicOp->isLogicalOp() || logicOp->isKnownToHaveBooleanValue()))
                            {
                                OS << "assert(";
                                TraverseStmt(logicOp->getLHS()->IgnoreParens());
                                OS << ", "; TraverseStmt(callExpr->getArg(0));
                                OS << ")";
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }
};
} // namespace


static
const TemplateArgument &getArgument(const TemplateArgument &A) { return A; }

static const TemplateArgument &getArgument(const TemplateArgumentLoc &A) {
  return A.getArgument();
}

template <typename TA>
void printDTemplateArgumentList(raw_ostream &OS, ArrayRef<TA> Args,
                                      const PrintingPolicy &Policy, bool SkipBrackets)
{
    if (!SkipBrackets)
        OS << "!(";
    bool FirstArg = true;
    for (const auto &Arg : Args)
    {
        // Print the argument into a string.
        SmallString<128> Buf;
        llvm::raw_svector_ostream ArgOS(Buf);
        const TemplateArgument &Argument = getArgument(Arg);
        if (Argument.getKind() == TemplateArgument::Pack)
        {
            if (Argument.pack_size() && !FirstArg)
                OS << ", ";
            printDTemplateArgumentList(ArgOS, Argument.getPackAsArray(), Policy);
        }
        else if (Argument.getKind() == TemplateArgument::Expression)
        {
            if (!FirstArg)
                OS << ", ";
            printPrettyD(Argument.getAsExpr(), OS, nullptr, Policy);
        }
        else
        {
            if (!FirstArg)
                OS << ", ";
            if (Argument.getKind() == TemplateArgument::ArgKind::Type)
                ArgOS << DlangBindGenerator::toDStyle(Argument.getAsType());
            else
                Argument.print(Policy, ArgOS);
        }

        OS << ArgOS.str();

        FirstArg = false;
    }
    if (!SkipBrackets)
        OS << ")";
}

void printPrettyD(const Stmt *stmt, raw_ostream &OS, PrinterHelper *Helper,
                     const PrintingPolicy &Policy, unsigned Indentation /*= 0*/,
                     const ASTContext *Context /*= nullptr*/,
                     FeedbackContext* feedback)
{
    CppDASTPrinterVisitor P(OS, Helper, Policy, Indentation, Context, feedback);
    P.TraverseStmt(const_cast<Stmt*>(stmt));
}

void printPrettyD(const CXXCtorInitializer *init, raw_ostream &OS, PrinterHelper *Helper,
                     const PrintingPolicy &Policy, unsigned Indentation /*= 0*/,
                     const ASTContext *Context /*= nullptr*/,
                     FeedbackContext* feedback)
{
    CppDASTPrinterVisitor P(OS, Helper, Policy, Indentation, Context, feedback);
    P.TraverseConstructorInitializer(const_cast<CXXCtorInitializer*>(init));
}
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "codegen/codegen.h"
#include "hir/hir.h"
#include "hir/hir_builder.h"
#include "interpret/interpreter.h"
#include "mir/mir.h"
#include "mir/mir_builder.h"
#include "mir/interpreter.h"
#include "parsing/ast.h"
#include "parsing/lexer.h"
#include "parsing/parser.h"
#include "parsing/preprocessor.h"
#include "parsing/token.h"

namespace {

void printUsage() {
    std::cout << "usage: ivyc [options] <file.ivy|file.cpp>\n"
                 "\n"
                 "Source files:\n"
                 "  .ivy        Ivy source (the safe C++ subset)\n"
                 "  .cpp/.cc/.cxx/.c  Legacy C/C++ source (migration)\n"
                 "\n"
                 "options:\n"
                 "  --tokens   print the token stream (debug)\n"
                 "  --ast      print the parsed AST (debug)\n"
                 "  --hir      print the analyzed HIR (debug)\n"
                 "  --mir      print the lowered MIR with lifetime annotations (debug)\n"
                 "  --llvm     emit LLVM IR to stdout\n"
                 "  --run      interpret the program via IvyInterpret v0.2 (MIR-based)\n"
                 "  --target <abi>  set the C++ ABI for name mangling:\n"
                 "               itanium (POSIX) or msvc (Windows)\n"
                 "               (default: auto-detect from host)\n"
                 "  -o <file>  write output to <file>:\n"
                 "               .ll  -> LLVM IR\n"
                 "               .i/.ii -> C++ source after #include expansion\n"
                 "  -I <dir>   add <dir> to the #include <...> search path (repeatable)\n"
                 "  -h, --help show this help\n";
}

// --- AST dumping (debug) ---

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void dumpAttrs(const std::vector<ivy::Attribute>& attrs, std::ostream& os) {
    for (const ivy::Attribute& a : attrs) {
        os << "[[ivy::" << a.name;
        if (!a.args.empty()) {
            os << "(";
            for (std::size_t i = 0; i < a.args.size(); ++i) {
                if (i) os << ", ";
                os << a.args[i];
            }
            os << ")";
        }
        os << "]]";
    }
}

void dumpType(const ivy::Type& t, std::ostream& os) {
    if (t.isConst) os << "const ";
    if (t.isUnsigned) os << "unsigned ";
    os << t.base;
    for (std::uint32_t i = 0; i < t.pointerDepth; ++i) os << "*";
}

// Forward declarations — lambda bodies need dumpStmt inside dumpExpr.
void dumpStmt(const ivy::Stmt& s, std::ostream& os, int depth);

void dumpExpr(const ivy::Expr& e, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    std::visit(Overloaded{
                   [&](const ivy::Expr::IntegerLit& v) { os << pad << "int " << v.value; },
                   [&](const ivy::Expr::FloatLit& v) { os << pad << "float " << v.value; },
                   [&](const ivy::Expr::StringLit& v) { os << pad << "string " << v.raw; },
                   [&](const ivy::Expr::CharLit& v) { os << pad << "char " << v.raw; },
                   [&](const ivy::Expr::BoolLit& v) { os << pad << "bool " << (v.value ? "true" : "false"); },
                   [&](const ivy::Expr::NullptrLit&) { os << pad << "nullptr"; },
                   [&](const ivy::Expr::IdentRef& v) { os << pad << "ident " << v.name; },
                   [&](const ivy::Expr::Unary& v) {
                       os << pad << (v.isPrefix ? "prefix " : "postfix ") << v.op << "\n";
                       dumpExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::Expr::Binary& v) {
                       os << pad << "binary " << v.op << "\n";
                       dumpExpr(*v.lhs, os, depth + 1);
                       dumpExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::Expr::Ternary& v) {
                       os << pad << "ternary ?:\n";
                       dumpExpr(*v.cond, os, depth + 1);
                       dumpExpr(*v.thenBranch, os, depth + 1);
                       dumpExpr(*v.elseBranch, os, depth + 1);
                   },
                   [&](const ivy::Expr::Call& v) {
                       os << pad << "call\n";
                       dumpExpr(*v.callee, os, depth + 1);
                       for (const auto& a : v.args) dumpExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::Expr::Index& v) {
                       os << pad << "index\n";
                       dumpExpr(*v.base, os, depth + 1);
                       dumpExpr(*v.index, os, depth + 1);
                   },
                   [&](const ivy::Expr::Member& v) {
                       os << pad << (v.isArrow ? "member ->" : "member .") << " " << v.name << "\n";
                       dumpExpr(*v.base, os, depth + 1);
                   },
                   [&](const ivy::Expr::Assign& v) {
                       os << pad << "assign " << v.op << "\n";
                       dumpExpr(*v.lhs, os, depth + 1);
                       dumpExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::Expr::New& v) {
                       os << pad << "new ";
                       dumpType(v.type, os);
                       os << "\n";
                       for (const auto& a : v.args) dumpExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::Expr::Delete& v) {
                       os << pad << (v.isArray ? "delete[]" : "delete") << "\n";
                       dumpExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::Expr::InitList& v) {
                       os << pad << "init-list (" << v.elements.size() << ")\n";
                       for (const auto& el : v.elements) {
                           if (el) dumpExpr(*el, os, depth + 1);
                           else os << (pad) << "  <zero-init>\n";
                       }
                   },
                   [&](const ivy::Expr::Lambda& v) {
                       os << pad << "lambda [";
                       for (std::size_t i = 0; i < v.captures.size(); ++i) {
                           if (i > 0) os << ", ";
                           if (v.captures[i].byRef) os << "&";
                           os << v.captures[i].name;
                       }
                       os << "](";
                       for (std::size_t i = 0; i < v.params.size(); ++i) {
                           if (i > 0) os << ", ";
                           dumpType(v.params[i].type, os);
                           if (!v.params[i].name.empty()) os << " " << v.params[i].name;
                       }
                       os << "]";
                       if (!v.returnType.base.empty()) {
                           os << " -> ";
                           dumpType(v.returnType, os);
                       }
                       os << "\n";
                       if (v.body) dumpStmt(*v.body, os, depth + 1);
                   },
               },
               e.node);
}

// Stmt::Compound cannot be copied (owns unique_ptr<Stmt>), so dump it via its
// own entry point; dumpStmt's Compound visitor delegates here.
void dumpCompound(const ivy::Stmt::Compound& c, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    os << pad << "block\n";
    for (const auto& st : c.stmts) dumpStmt(*st, os, depth + 1);
}

void dumpStmt(const ivy::Stmt& s, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    std::visit(Overloaded{
                   [&](const ivy::Stmt::Compound& v) { dumpCompound(v, os, depth); },
                   [&](const ivy::Stmt::Decl& v) {
                       os << pad << "decl ";
                       dumpType(v.type, os);
                       os << " " << v.name;
                       if (v.init) {
                           os << " =\n";
                           dumpExpr(*v.init, os, depth + 1);
                       } else {
                           os << "\n";
                       }
                   },
                   [&](const ivy::Stmt::If& v) {
                       os << pad << "if\n";
                       dumpExpr(*v.cond, os, depth + 1);
                       os << pad << "  then\n";
                       dumpStmt(*v.thenBranch, os, depth + 1);
                       if (v.elseBranch) {
                           os << pad << "  else\n";
                           dumpStmt(*v.elseBranch, os, depth + 1);
                       }
                   },
                   [&](const ivy::Stmt::While& v) {
                       os << pad << "while\n";
                       dumpExpr(*v.cond, os, depth + 1);
                       dumpStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::Stmt::DoWhile& v) {
                       os << pad << "do-while\n";
                       dumpStmt(*v.body, os, depth + 1);
                       dumpExpr(*v.cond, os, depth + 1);
                   },
                   [&](const ivy::Stmt::For& v) {
                       os << pad << "for\n";
                       os << pad << "  init\n";
                       dumpStmt(*v.init, os, depth + 1);
                       os << pad << "  cond\n";
                       if (v.cond) dumpExpr(*v.cond, os, depth + 1);
                       os << pad << "  incr\n";
                       if (v.incr) dumpExpr(*v.incr, os, depth + 1);
                       os << pad << "  body\n";
                       dumpStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::Stmt::Return& v) {
                       os << pad << "return";
                       if (v.value) {
                           os << "\n";
                           dumpExpr(*v.value, os, depth + 1);
                       } else {
                           os << "\n";
                       }
                   },
                   [&](const ivy::Stmt::Break&) { os << pad << "break\n"; },
                   [&](const ivy::Stmt::Continue&) { os << pad << "continue\n"; },
                   [&](const ivy::Stmt::ExprStmt& v) {
                       os << pad << "expr\n";
                       dumpExpr(*v.value, os, depth + 1);
                   },
                   [&](const ivy::Stmt::Unsafe& v) {
                       os << pad << "unsafe\n";
                       dumpStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::Stmt::Null&) { os << pad << "null\n"; },
               },
               s.node);
}

void dumpAst(const ivy::TranslationUnit& tu, std::ostream& os) {
    for (const ivy::Function& fn : tu.functions) {
        os << "function ";
        if (fn.isExternC) os << "[extern \"C\"] ";
        dumpAttrs(fn.attrs, os);
        os << "\n  return ";
        dumpType(fn.returnType, os);
        os << " " << fn.name << "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) os << ", ";
            dumpType(fn.params[i].type, os);
            if (!fn.params[i].name.empty()) os << " " << fn.params[i].name;
            if (!fn.params[i].attrs.empty()) {
                os << " ";
                dumpAttrs(fn.params[i].attrs, os);
            }
        }
        os << ")\n";
        if (fn.body) {
            dumpCompound(*fn.body, os, 1);
        } else {
            os << "  (declaration only)\n";
        }
        os << "\n";
    }
}

// --- HIR dumping (debug) ---

void dumpHirType(const ivy::hir::Type& t, std::ostream& os) {
    if (t.isConst) os << "const ";
    if (t.isUnsigned) os << "unsigned ";
    os << t.base;
    for (std::uint32_t i = 0; i < t.pointerDepth; ++i) os << "*";
    if (t.isReference) os << "&";
}

void dumpHirExpr(const ivy::hir::Expr& e, std::ostream& os, int depth);

void dumpHirExprHeader(const ivy::hir::Expr& e, std::ostream& os, int depth,
                       std::string label) {
    const std::string pad(depth * 2, ' ');
    os << pad << label << " : ";
    dumpHirType(e.type, os);
    os << "\n";
}

void dumpHirExpr(const ivy::hir::Expr& e, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    std::visit(Overloaded{
                   [&](const ivy::hir::Expr::IntegerLit& v) { os << pad << "int " << v.value << "\n"; },
                   [&](const ivy::hir::Expr::FloatLit& v) { os << pad << "float " << v.value << "\n"; },
                   [&](const ivy::hir::Expr::StringLit& v) { os << pad << "string " << v.raw << "\n"; },
                   [&](const ivy::hir::Expr::CharLit& v) { os << pad << "char " << v.raw << "\n"; },
                   [&](const ivy::hir::Expr::BoolLit& v) { os << pad << "bool " << (v.value ? "true" : "false") << "\n"; },
                   [&](const ivy::hir::Expr::NullptrLit&) { os << pad << "nullptr\n"; },
                   [&](const ivy::hir::Expr::IdentRef& v) { os << pad << "ident " << v.name << "\n"; },
                   [&](const ivy::hir::Expr::Unary& v) {
                       dumpHirExprHeader(e, os, depth,
                                         std::string(v.isPrefix ? "prefix '" : "postfix '") +
                                             std::string(v.op) + "'");
                       dumpHirExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Binary& v) {
                       dumpHirExprHeader(e, os, depth, "binary '" + std::string(v.op) + "'");
                       dumpHirExpr(*v.lhs, os, depth + 1);
                       dumpHirExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Ternary& v) {
                       dumpHirExprHeader(e, os, depth, "ternary ?:");
                       dumpHirExpr(*v.cond, os, depth + 1);
                       dumpHirExpr(*v.thenBranch, os, depth + 1);
                       dumpHirExpr(*v.elseBranch, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Call& v) {
                       dumpHirExprHeader(e, os, depth, "call");
                       for (const auto& a : v.args) dumpHirExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Index& v) {
                       dumpHirExprHeader(e, os, depth, "index");
                       dumpHirExpr(*v.base, os, depth + 1);
                       dumpHirExpr(*v.index, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Member& v) {
                       os << pad << (v.isArrow ? "member ->" : "member .") << " " << v.name << "\n";
                       dumpHirExpr(*v.base, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Assign& v) {
                       dumpHirExprHeader(e, os, depth, "assign '" + std::string(v.op) + "'");
                       dumpHirExpr(*v.lhs, os, depth + 1);
                       dumpHirExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::New& v) {
                       os << pad << "new : ";
                       dumpHirType(e.type, os);
                       os << "\n";
                       for (const auto& a : v.args) dumpHirExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Delete& v) {
                       os << pad << (v.isArray ? "delete[]" : "delete") << "\n";
                       dumpHirExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::InitList& v) {
                       os << pad << "init-list : ";
                       dumpHirType(e.type, os);
                       os << " (" << v.elements.size() << ")\n";
                       for (const auto& el : v.elements) dumpHirExpr(*el, os, depth + 1);
                   },
                   [&](const ivy::hir::Expr::Lambda& v) {
                       os << pad << "lambda : ";
                       dumpHirType(e.type, os);
                       os << " func=" << v.funcName << " closure=" << v.closureType
                          << " (" << v.captureInits.size() << " captures)\n";
                       for (const auto& ci : v.captureInits)
                           if (ci) dumpHirExpr(*ci, os, depth + 1);
                   },
               },
               e.node);
}

void dumpHirStmt(const ivy::hir::Stmt& s, std::ostream& os, int depth);

void dumpHirCompound(const ivy::hir::Stmt::Compound& c, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    os << pad << "block\n";
    for (const auto& st : c.stmts) dumpHirStmt(*st, os, depth + 1);
}

void dumpHirStmt(const ivy::hir::Stmt& s, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    std::visit(Overloaded{
                   [&](const ivy::hir::Stmt::Compound& v) { dumpHirCompound(v, os, depth); },
                   [&](const ivy::hir::Stmt::Decl& v) {
                       os << pad << "decl ";
                       dumpHirType(v.type, os);
                       os << " " << v.name;
                       if (v.init) {
                           os << " =\n";
                           dumpHirExpr(*v.init, os, depth + 1);
                       } else {
                           os << "\n";
                       }
                   },
                   [&](const ivy::hir::Stmt::If& v) {
                       os << pad << "if\n";
                       dumpHirExpr(*v.cond, os, depth + 1);
                       dumpHirStmt(*v.thenBranch, os, depth + 1);
                       if (v.elseBranch) dumpHirStmt(*v.elseBranch, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::While& v) {
                       os << pad << "while\n";
                       dumpHirExpr(*v.cond, os, depth + 1);
                       dumpHirStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::DoWhile& v) {
                       os << pad << "do-while\n";
                       dumpHirStmt(*v.body, os, depth + 1);
                       dumpHirExpr(*v.cond, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::For& v) {
                       os << pad << "for\n";
                       dumpHirStmt(*v.init, os, depth + 1);
                       if (v.cond) dumpHirExpr(*v.cond, os, depth + 1);
                       if (v.incr) dumpHirExpr(*v.incr, os, depth + 1);
                       dumpHirStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::Return& v) {
                       os << pad << "return";
                       if (v.value) {
                           os << "\n";
                           dumpHirExpr(*v.value, os, depth + 1);
                       } else {
                           os << "\n";
                       }
                   },
                   [&](const ivy::hir::Stmt::Break&) { os << pad << "break\n"; },
                   [&](const ivy::hir::Stmt::Continue&) { os << pad << "continue\n"; },
                   [&](const ivy::hir::Stmt::ExprStmt& v) {
                       os << pad << "expr\n";
                       dumpHirExpr(*v.value, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::Unsafe& v) {
                       os << pad << "unsafe\n";
                       dumpHirStmt(*v.body, os, depth + 1);
                   },
                   [&](const ivy::hir::Stmt::Null&) { os << pad << "null\n"; },
               },
               s.node);
}

void dumpHir(const ivy::hir::TranslationUnit& tu, std::ostream& os) {
    for (const auto& fn : tu.functions) {
        os << "function " << fn->name;
        if (fn->isExternC) os << " [extern \"C\"]";
        os << "\n  return ";
        dumpHirType(fn->returnType, os);
        os << "(";
        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            if (i) os << ", ";
            dumpHirType(fn->params[i].type, os);
            if (!fn->params[i].name.empty()) os << " " << fn->params[i].name;
            if (!fn->params[i].lifetime.empty()) os << " [lt:" << fn->params[i].lifetime << "]";
        }
        os << ")\n";
        if (!fn->lifetimes.empty()) {
            os << "  lifetimes:";
            for (const auto& l : fn->lifetimes) os << " " << l.name;
            os << "\n";
        }
        if (!fn->returnLifetime.empty()) os << "  return lifetime: " << fn->returnLifetime << "\n";
        if (fn->body) {
            dumpHirCompound(*fn->body, os, 1);
        } else {
            os << "  (declaration only)\n";
        }
        os << "\n";
    }
}

// --- MIR dumping (debug) ---

std::string mirLifetimeSuffix(const ivy::mir::Lifetime& lt) {
    using K = ivy::mir::Lifetime::Kind;
    switch (lt.kind) {
        case K::None: return "";
        case K::Named: return std::string(lt.name);
        case K::Static: return "static";
        case K::Local: return "local";
        case K::Unknown: return "unknown";
    }
    return "";
}

void dumpMirExpr(const ivy::mir::Expr& e, std::ostream& os, int depth);

void dumpMirExprHeader(const ivy::mir::Expr& e, std::ostream& os, int depth,
                       std::string label) {
    const std::string pad(depth * 2, ' ');
    os << pad << label << " : ";
    dumpHirType(e.type, os);
    const std::string lt = mirLifetimeSuffix(e.lifetime);
    if (!lt.empty()) os << " [lt: " << lt << "]";
    os << "\n";
}

void dumpMirExpr(const ivy::mir::Expr& e, std::ostream& os, int depth) {
    const std::string pad(depth * 2, ' ');
    const std::string lt = mirLifetimeSuffix(e.lifetime);
    const std::string lts = lt.empty() ? "" : " [lt: " + lt + "]";
    std::visit(Overloaded{
                   [&](const ivy::mir::Expr::IntegerLit& v) { os << pad << "int " << v.value << lts << "\n"; },
                   [&](const ivy::mir::Expr::FloatLit& v) { os << pad << "float " << v.value << lts << "\n"; },
                   [&](const ivy::mir::Expr::StringLit& v) { os << pad << "string " << v.raw << lts << "\n"; },
                   [&](const ivy::mir::Expr::CharLit& v) { os << pad << "char " << v.raw << lts << "\n"; },
                   [&](const ivy::mir::Expr::BoolLit& v) { os << pad << "bool " << (v.value ? "true" : "false") << lts << "\n"; },
                   [&](const ivy::mir::Expr::NullptrLit&) { os << pad << "nullptr" << lts << "\n"; },
                   [&](const ivy::mir::Expr::IdentRef& v) { os << pad << "ident " << v.name << lts << "\n"; },
                   [&](const ivy::mir::Expr::Unary& v) {
                       dumpMirExprHeader(e, os, depth,
                                         std::string(v.isPrefix ? "prefix '" : "postfix '") +
                                             std::string(v.op) + "'");
                       dumpMirExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Binary& v) {
                       dumpMirExprHeader(e, os, depth, "binary '" + std::string(v.op) + "'");
                       dumpMirExpr(*v.lhs, os, depth + 1);
                       dumpMirExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Ternary& v) {
                       dumpMirExprHeader(e, os, depth, "ternary ?:");
                       dumpMirExpr(*v.cond, os, depth + 1);
                       dumpMirExpr(*v.thenBranch, os, depth + 1);
                       dumpMirExpr(*v.elseBranch, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Call& v) {
                       dumpMirExprHeader(e, os, depth, "call");
                       for (const auto& a : v.args) dumpMirExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Index& v) {
                       dumpMirExprHeader(e, os, depth, "index");
                       dumpMirExpr(*v.base, os, depth + 1);
                       dumpMirExpr(*v.index, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Member& v) {
                       dumpMirExprHeader(e, os, depth,
                                         std::string("member ") + (v.isArrow ? "->" : ".") +
                                             std::string(v.name));
                       dumpMirExpr(*v.base, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Assign& v) {
                       dumpMirExprHeader(e, os, depth, "assign '" + std::string(v.op) + "'");
                       dumpMirExpr(*v.lhs, os, depth + 1);
                       dumpMirExpr(*v.rhs, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::New& v) {
                       os << pad << "new : ";
                       dumpHirType(e.type, os);
                       os << lts << "\n";
                       for (const auto& a : v.args) dumpMirExpr(*a, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Delete& v) {
                       os << pad << (v.isArray ? "delete[]" : "delete") << lts << "\n";
                       dumpMirExpr(*v.operand, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::InitList& v) {
                       os << pad << "init-list : ";
                       dumpHirType(e.type, os);
                       os << lts << " (" << v.elements.size() << ")\n";
                       for (const auto& el : v.elements) dumpMirExpr(*el, os, depth + 1);
                   },
                   [&](const ivy::mir::Expr::Lambda& v) {
                       os << pad << "lambda : ";
                       dumpHirType(e.type, os);
                       os << lts << " func=" << v.funcName << " closure=" << v.closureType
                          << " (" << v.captureInits.size() << " captures)\n";
                       for (const auto& ci : v.captureInits)
                           if (ci) dumpMirExpr(*ci, os, depth + 1);
                   },
               },
               e.node);
}

void dumpMirBlock(const ivy::mir::Block& b, std::ostream& os, int index, int depth) {
    const std::string pad(depth * 2, ' ');
    os << pad << "block " << index << "\n";
    for (const auto& inst : b.insts) {
        const std::string mark = inst->inUnsafe ? " [unsafe]" : "";
        switch (inst->kind) {
            case ivy::mir::Inst::Kind::Alloca: {
                const auto& a = std::get<ivy::mir::Inst::Alloca>(inst->node);
                os << pad << "  alloca ";
                dumpHirType(a.type, os);
                os << " " << a.var << mark << "\n";
                if (a.init) dumpMirExpr(*a.init, os, depth + 2);
                break;
            }
            case ivy::mir::Inst::Kind::Store: {
                const auto& st = std::get<ivy::mir::Inst::Store>(inst->node);
                os << pad << "  store" << mark << "\n";
                if (st.target) dumpMirExpr(*st.target, os, depth + 2);
                if (st.value) dumpMirExpr(*st.value, os, depth + 2);
                break;
            }
            case ivy::mir::Inst::Kind::Eval: {
                const auto& ev = std::get<ivy::mir::Inst::Eval>(inst->node);
                os << pad << "  eval" << mark << "\n";
                if (ev.value) dumpMirExpr(*ev.value, os, depth + 2);
                break;
            }
            case ivy::mir::Inst::Kind::Ret: {
                const auto& r = std::get<ivy::mir::Inst::Ret>(inst->node);
                os << pad << "  ret" << mark << "\n";
                if (r.value) dumpMirExpr(*r.value, os, depth + 2);
                break;
            }
            case ivy::mir::Inst::Kind::CondBranch: {
                const auto& cb = std::get<ivy::mir::Inst::CondBranch>(inst->node);
                os << pad << "  condbranch" << mark << "\n";
                if (cb.cond) dumpMirExpr(*cb.cond, os, depth + 2);
                break;
            }
            case ivy::mir::Inst::Kind::Jump:
                os << pad << "  jump" << mark << "\n";
                break;
        }
    }
}

void dumpMir(const ivy::mir::TranslationUnit& tu, std::ostream& os) {
    for (const auto& fn : tu.functions) {
        os << "function " << fn->name;
        if (fn->isExternC) os << " [extern \"C\"]";
        os << "\n  return ";
        dumpHirType(fn->returnType, os);
        os << "(";
        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            if (i) os << ", ";
            dumpHirType(fn->params[i].type, os);
            if (!fn->params[i].name.empty()) os << " " << fn->params[i].name;
            if (!fn->params[i].lifetime.empty()) os << " [lt:" << fn->params[i].lifetime << "]";
        }
        os << ")\n";
        if (!fn->lifetimes.empty()) {
            os << "  lifetimes:";
            for (const auto& l : fn->lifetimes) os << " " << l.name;
            os << "\n";
        }
        if (!fn->returnLifetime.empty()) os << "  return lifetime: " << fn->returnLifetime << "\n";
        for (std::size_t i = 0; i < fn->blocks.size(); ++i) {
            dumpMirBlock(*fn->blocks[i], os, static_cast<int>(i), 1);
        }
        os << "\n";
    }
}

// --- driver ---

// Accepted source-file extensions. `.ivy` marks Ivy code (the new
// safe subset); `.cpp`/`.cc`/`.cxx`/`.c` are accepted for compatibility
// with legacy C/C++ files being migrated to Ivy.
bool isKnownSourceExtension(std::string_view ext) {
    return ext == "ivy" || ext == "cpp" || ext == "cc" ||
           ext == "cxx" || ext == "c";
}

std::string_view getExtension(std::string_view path) {
    const auto pos = path.find_last_of('.');
    if (pos == std::string_view::npos) return {};
    return path.substr(pos + 1);
}

bool isIvyFile(const std::filesystem::path& path) {
    return getExtension(path.string()) == "ivy";
}

// Case-sensitive check of a path's extension (without the dot).
bool hasExtension(const std::string& path, std::string_view ext) {
    const auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    const std::string_view got = std::string_view(path).substr(pos + 1);
    return got == ext;
}

int run(const std::filesystem::path& path, bool showTokens, bool showAst, bool showHir,
        bool showMir, bool showLlvm, bool doRun, const std::string& outPath,
        const std::vector<std::filesystem::path>& includePaths,
        std::optional<ivy::CodeGen::Platform> targetPlatform) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "ivyc: error: cannot open file '" << path << "'\n";
        return 2;
    }
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    ivy::Lexer lexer(source);
    std::vector<ivy::Token> tokens = lexer.tokenize();

    bool failed = false;
    for (const ivy::Diagnostic& d : lexer.diagnostics()) {
        std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
        failed = true;
    }
    if (failed) return 1;

    // Decide whether the user asked for preprocessed C++ output (.i / .ii).
    // When `-o file.i` or `-o file.ii` is given, we run the preprocessor
    // and write the reconstructed source text instead of going through
    // parser/HIR/MIR/codegen — mirroring `g++ -E`.
    const bool emitPreprocessed = !outPath.empty() && hasExtension(outPath, "i") ||
                                  !outPath.empty() && hasExtension(outPath, "ii");

    // Run the preprocessor (#include expansion) unconditionally — the
    // parser no longer accepts `#` tokens as directives. The search paths
    // are exactly those passed via -I; there is no implicit system path.
    ivy::Preprocessor pp(std::move(tokens), path, includePaths);
    tokens = pp.run();
    for (const ivy::Diagnostic& d : pp.diagnostics()) {
        std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
        failed = true;
    }
    if (failed) return 1;

    if (emitPreprocessed) {
        const std::string text = pp.reconstructSource();
        std::ofstream ofs(outPath);
        if (!ofs) {
            std::cerr << "ivyc: error: cannot open output file '" << outPath << "'\n";
            return 2;
        }
        ofs << text;
        return 0;
    }

    if (showTokens) {
        for (const ivy::Token& t : tokens) {
            std::cout << t.line << ":" << t.col << "\t" << ivy::tokenKindName(t.kind);
            if (t.kind == ivy::TokenKind::EndOfFile) break;
            if (!t.lexeme.empty()) std::cout << "\t'" << t.lexeme << "'";
            std::cout << "\n";
        }
        return 0;
    }

    ivy::Parser parser(tokens, pp.cnumberEnabled());
    std::unique_ptr<ivy::TranslationUnit> tu = parser.parse();
    for (const ivy::Diagnostic& d : parser.diagnostics()) {
        std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
        failed = true;
    }
    if (failed) return 1;

    if (showAst && tu) dumpAst(*tu, std::cout);

    ivy::HirBuilder builder(*tu);
    std::unique_ptr<ivy::hir::TranslationUnit> hir = builder.build();
    for (const ivy::Diagnostic& d : builder.diagnostics()) {
        std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
        failed = true;
    }
    if (failed) return 1;

    if (showHir && hir) dumpHir(*hir, std::cout);

    ivy::MirBuilder mirBuilder(*hir);
    std::unique_ptr<ivy::mir::TranslationUnit> mir = mirBuilder.build();
    for (const ivy::Diagnostic& d : mirBuilder.diagnostics()) {
        std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message << "\n";
        failed = true;
    }
    if (failed) return 1;

    if (showMir && mir) dumpMir(*mir, std::cout);

    // --run mode: interpret the MIR directly via IvyInterpret v0.2.
    // No codegen / native compiler needed.
    if (doRun && mir) {
        ivy::mir::Interpreter interp(*mir);
        ivy::mir::Value result = interp.callMain();
        for (const auto& d : interp.diagnostics()) {
            std::cerr << path << ":" << d.line << ":" << d.col
                      << ": error: " << d.message << "\n";
        }
        if (interp.failed()) return 1;
        // If main returned a non-void exit code, forward it.
        if (result.isInt()) return static_cast<int>(result.asInt());
        return 0;
    }

    if (showLlvm && mir) {
        ivy::CodeGen cg(*mir);
        if (targetPlatform) cg.setPlatform(*targetPlatform);
        bool ok = false;
        if (!outPath.empty()) {
            std::ofstream ofs(outPath);
            if (!ofs) {
                std::cerr << "ivyc: error: cannot open output file '" << outPath << "'\n";
                return 2;
            }
            ok = cg.generate(ofs);
        } else {
            ok = cg.generate(std::cout);
        }
        for (const ivy::Diagnostic& d : cg.diagnostics()) {
            std::cerr << path << ":" << d.line << ":" << d.col << ": error: " << d.message
                      << "\n";
        }
        return ok ? 0 : 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path file;
    std::string fileArg;  // original argv string for extension checks
    bool showTokens = false;
    bool showAst = false;
    bool showHir = false;
    bool showMir = false;
    bool showLlvm = false;
    bool doRun = false;
    std::string outPath;
    std::vector<std::filesystem::path> includePaths;
    std::optional<ivy::CodeGen::Platform> targetPlatform;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--tokens") {
            showTokens = true;
        } else if (arg == "--ast") {
            showAst = true;
        } else if (arg == "--hir") {
            showHir = true;
        } else if (arg == "--mir") {
            showMir = true;
        } else if (arg == "--llvm") {
            showLlvm = true;
        } else if (arg == "--run") {
            doRun = true;
        } else if (arg == "--target") {
            if (i + 1 >= argc) {
                std::cerr << "ivyc: error: option '--target' requires an ABI name\n";
                return 2;
            }
            const std::string_view t(argv[++i]);
            if (t == "itanium") {
                targetPlatform = ivy::CodeGen::Platform::Itanium;
            } else if (t == "msvc") {
                targetPlatform = ivy::CodeGen::Platform::MSVC;
            } else {
                std::cerr << "ivyc: error: unknown target ABI '" << t
                          << "' (expected 'itanium' or 'msvc')\n";
                return 2;
            }
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "ivyc: error: option '-o' requires a file name\n";
                return 2;
            }
            outPath = argv[++i];
        } else if (arg == "-I") {
            if (i + 1 >= argc) {
                std::cerr << "ivyc: error: option '-I' requires a directory\n";
                return 2;
            }
            includePaths.emplace_back(argv[++i]);
        } else if (arg.starts_with("-I")) {
            // `-Idir` joined form.
            includePaths.emplace_back(arg.substr(2));
        } else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ivyc: error: unknown option '" << arg << "'\n";
            printUsage();
            return 2;
        } else {
            file = arg;
            fileArg = argv[i];
        }
    }

    if (file.empty()) {
        printUsage();
        return 2;
    }

    // Validate source extension — warn (not error) on unknown extensions.
    // `.ivy` is the canonical Ivy extension; `.cpp`/`.cc`/`.cxx`/`.c` are
    // accepted for legacy migration. Other extensions are warned about but
    // still processed (the user may know what they are doing).
    const std::string_view fileExt = getExtension(fileArg);
    if (!fileExt.empty() && !isKnownSourceExtension(fileExt)) {
        std::cerr << "ivyc: warning: unrecognized file extension '." << fileExt
                  << "' — expected .ivy, .cpp, .cc, .cxx, or .c\n";
    }

    return run(file, showTokens, showAst, showHir, showMir, showLlvm, doRun, outPath,
               includePaths, targetPlatform);
}

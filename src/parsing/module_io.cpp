// 9.2: Module interface file (.ivm) writer and reader.

#include "parsing/module_io.h"

#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>

namespace ivy {

namespace {

// Stable string pool for keeping string_views alive after deserialization.
// We use a deque<string> because deque does not invalidate references
// to its elements on push_back (unlike vector<string> which may
// reallocate and invalidate all string_views pointing into its storage).
std::deque<std::string> g_stringPool;

std::string_view intern(std::string_view sv) {
    g_stringPool.emplace_back(sv);
    return g_stringPool.back();
}

// Serialize a Type to a compact string representation.
// Format: base[:unsigned][:const][:ref][:ptr<depth>][:arr<size>][:tpl<args>]
std::string serializeType(const Type& t) {
    std::string s = std::string(t.base);
    if (t.isUnsigned) s += ":unsigned";
    if (t.isConst) s += ":const";
    if (t.isReference) s += ":ref";
    if (t.pointerDepth > 0) s += ":ptr" + std::to_string(t.pointerDepth);
    if (t.arraySize > 0) s += ":arr" + std::to_string(t.arraySize);
    for (const auto& arg : t.tplArgs) {
        s += ":tpl{" + serializeType(arg) + "}";
    }
    return s;
}

// Parse a serialized type string back into a Type struct.
Type deserializeType(const std::string& s) {
    Type t;
    std::size_t pos = 0;
    // base is everything up to first ':'
    std::size_t colon = s.find(':', pos);
    if (colon == std::string::npos) {
        t.base = intern(s);
        return t;
    }
    t.base = intern(s.substr(0, colon));
    pos = colon + 1;
    while (pos < s.size()) {
        // Check for tpl{...}
        if (s.compare(pos, 4, "tpl{") == 0) {
            int depth = 1;
            std::size_t end = pos + 4;
            while (end < s.size() && depth > 0) {
                if (s[end] == '{') ++depth;
                else if (s[end] == '}') --depth;
                if (depth > 0) ++end;
            }
            t.tplArgs.push_back(deserializeType(s.substr(pos + 4, end - pos - 4)));
            if (end + 1 < s.size() && s[end + 1] == ':') {
                pos = end + 2;
            } else {
                break;
            }
            continue;
        }
        colon = s.find(':', pos);
        std::string part = (colon == std::string::npos)
                           ? s.substr(pos) : s.substr(pos, colon - pos);
        if (part == "unsigned") t.isUnsigned = true;
        else if (part == "const") t.isConst = true;
        else if (part == "ref") t.isReference = true;
        else if (part.starts_with("ptr")) t.pointerDepth = std::stoul(part.substr(3));
        else if (part.starts_with("arr")) t.arraySize = std::stoul(part.substr(3));
        pos = (colon == std::string::npos) ? s.size() : colon + 1;
    }
    return t;
}

// Escape a string for the .ivm format (replace spaces/newlines).
std::string escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == ' ') r += "\\s";
        else r += c;
    }
    return r;
}

// Unescape a string from the .ivm format.
std::string unescape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i+1] == 'n') { r += '\n'; ++i; }
            else if (s[i+1] == 'r') { r += '\r'; ++i; }
            else if (s[i+1] == 's') { r += ' '; ++i; }
            else r += s[i];
        } else {
            r += s[i];
        }
    }
    return r;
}

}  // namespace

bool writeModuleInterface(const TranslationUnit& tu,
                          const std::filesystem::path& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::cerr << "ivyc: error: cannot write module interface to '"
                  << outputPath.string() << "'\n";
        return false;
    }

    // Header
    out << "IVYMOD\x01\n";
    out << "module " << tu.moduleName << "\n";

    // Exported functions (signatures only, no bodies)
    for (const Function& f : tu.functions) {
        if (!f.isExported) continue;
        out << "func " << f.name;
        out << ' ' << serializeType(f.returnType);
        out << ' ' << f.params.size();
        for (const auto& p : f.params) {
            out << ' ' << escape(serializeType(p.type))
                << ':' << escape(std::string(p.name));
        }
        out << ' ' << (f.isExternC ? 1 : 0);
        out << ' ' << (f.isConstexpr ? 1 : 0);
        out << ' ' << (f.isConsteval ? 1 : 0);
        out << ' ' << (f.isOperator ? 1 : 0);
        if (f.isOperator) out << ' ' << escape(f.operatorSymbol);
        out << '\n';
    }

    // Exported structs
    for (const StructDecl& sd : tu.structs) {
        if (!sd.isExported) continue;
        out << "struct " << sd.name << ' ' << sd.fields.size();
        for (const auto& field : sd.fields) {
            out << ' ' << escape(serializeType(field.type))
                << ':' << escape(std::string(field.name));
        }
        out << ' ' << sd.bases.size();
        for (const auto& base : sd.bases) {
            out << ' ' << escape(serializeType(base.type));
        }
        out << ' ' << (sd.isClass ? 1 : 0);
        out << '\n';
    }

    // Exported enums
    for (const EnumDecl& ed : tu.enums) {
        if (!ed.isExported) continue;
        out << "enum " << ed.name << ' ' << (ed.isScoped ? 1 : 0);
        out << ' ' << escape(serializeType(ed.underlyingType));
        out << ' ' << ed.enumerators.size();
        for (const auto& e : ed.enumerators) {
            out << ' ' << escape(std::string(e.name));
            // Try to extract integer value from the enumerator's Expr
            long long val = -1;
            if (e.value && std::holds_alternative<Expr::IntegerLit>(e.value->node)) {
                val = std::get<Expr::IntegerLit>(e.value->node).value;
            }
            out << '=' << val;
        }
        out << '\n';
    }

    // Type aliases
    for (const UsingDecl& ud : tu.usingDecls) {
        out << "using " << ud.name << ' ' << escape(serializeType(ud.targetType)) << '\n';
    }

    // Concepts
    for (const ConceptDecl& cd : tu.concepts) {
        out << "concept " << cd.name << ' ' << cd.paramName << ' '
            << cd.requirements.size();
        for (const auto& req : cd.requirements) {
            out << ' ' << escape(req);
        }
        out << '\n';
    }

    return true;
}

bool readModuleInterface(TranslationUnit& tu,
                         const std::filesystem::path& ivmPath) {
    std::ifstream in(ivmPath, std::ios::binary);
    if (!in) {
        std::cerr << "ivyc: error: cannot read module interface '"
                  << ivmPath.string() << "'\n";
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    std::istringstream ss(content);
    std::string line;

    // Check magic
    if (!std::getline(ss, line) || line != "IVYMOD\x01") {
        std::cerr << "ivyc: error: invalid module interface file '"
                  << ivmPath.string() << "' (bad magic)\n";
        return false;
    }

    // Read module name
    if (!std::getline(ss, line)) return false;
    if (line.starts_with("module ")) {
        tu.moduleName = line.substr(7);
    }

    // Read declarations
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        if (kind == "func") {
            Function f;
            f.body = nullptr;  // declaration only
            f.isExported = true;  // .ivm contains only exports
            std::string name;
            ls >> name;
            f.name = intern(name);
            std::string retTypeStr;
            ls >> retTypeStr;
            f.returnType = deserializeType(unescape(retTypeStr));
            std::size_t paramCount;
            ls >> paramCount;
            for (std::size_t i = 0; i < paramCount; ++i) {
                std::string paramStr;
                ls >> paramStr;
                paramStr = unescape(paramStr);
                Param p;
                auto colon = paramStr.find(':');
                if (colon != std::string::npos) {
                    p.type = deserializeType(paramStr.substr(0, colon));
                    p.name = intern(paramStr.substr(colon + 1));
                } else {
                    p.type = deserializeType(paramStr);
                }
                f.params.push_back(std::move(p));
            }
            int isExternC, isConstexpr, isConsteval, isOperator;
            ls >> isExternC >> isConstexpr >> isConsteval >> isOperator;
            f.isExternC = isExternC;
            f.isConstexpr = isConstexpr;
            f.isConsteval = isConsteval;
            f.isOperator = isOperator;
            if (isOperator) {
                std::string sym;
                ls >> sym;
                f.operatorSymbol = unescape(sym);
            }
            tu.functions.push_back(std::move(f));
        } else if (kind == "struct") {
            StructDecl sd;
            sd.isExported = true;  // .ivm contains only exports
            std::string name;
            ls >> name;
            sd.name = intern(name);
            std::size_t fieldCount;
            ls >> fieldCount;
            for (std::size_t i = 0; i < fieldCount; ++i) {
                std::string fieldStr;
                ls >> fieldStr;
                fieldStr = unescape(fieldStr);
                Field field;
                auto colon = fieldStr.find(':');
                if (colon != std::string::npos) {
                    field.type = deserializeType(fieldStr.substr(0, colon));
                    field.name = intern(fieldStr.substr(colon + 1));
                } else {
                    field.type = deserializeType(fieldStr);
                }
                sd.fields.push_back(std::move(field));
            }
            std::size_t baseCount;
            ls >> baseCount;
            for (std::size_t i = 0; i < baseCount; ++i) {
                std::string baseStr;
                ls >> baseStr;
                baseStr = unescape(baseStr);
                BaseClass bc;
                bc.type = deserializeType(baseStr);
                sd.bases.push_back(std::move(bc));
            }
            int isClass;
            ls >> isClass;
            sd.isClass = isClass;
            tu.structs.push_back(std::move(sd));
        } else if (kind == "enum") {
            EnumDecl ed;
            ed.isExported = true;  // .ivm contains only exports
            std::string name;
            ls >> name;
            ed.name = intern(name);
            int isScoped;
            ls >> isScoped;
            ed.isScoped = isScoped;
            std::string underlyingStr;
            ls >> underlyingStr;
            ed.underlyingType = deserializeType(unescape(underlyingStr));
            std::size_t count;
            ls >> count;
            for (std::size_t i = 0; i < count; ++i) {
                std::string enumStr;
                ls >> enumStr;
                enumStr = unescape(enumStr);
                Enumerator e;
                auto eq = enumStr.find('=');
                if (eq != std::string::npos) {
                    e.name = intern(enumStr.substr(0, eq));
                    // Value is stored as text but we don't reconstruct
                    // the Expr — HIR builder will assign sequential values.
                    e.value = nullptr;
                } else {
                    e.name = intern(enumStr);
                }
                ed.enumerators.push_back(std::move(e));
            }
            tu.enums.push_back(std::move(ed));
        } else if (kind == "using") {
            UsingDecl ud;
            std::string name;
            ls >> name;
            ud.name = intern(name);
            std::string targetStr;
            ls >> targetStr;
            ud.targetType = deserializeType(unescape(targetStr));
            tu.usingDecls.push_back(std::move(ud));
        } else if (kind == "concept") {
            ConceptDecl cd;
            std::string name;
            ls >> name;
            cd.name = intern(name);
            std::string paramName;
            ls >> paramName;
            cd.paramName = paramName;
            std::size_t reqCount;
            ls >> reqCount;
            for (std::size_t i = 0; i < reqCount; ++i) {
                std::string req;
                ls >> req;
                cd.requirements.push_back(unescape(req));
            }
            tu.concepts.push_back(std::move(cd));
        }
    }

    return true;
}

std::filesystem::path resolveModulePath(std::string_view moduleName,
    const std::vector<std::filesystem::path>& searchDirs) {
    for (const auto& dir : searchDirs) {
        auto candidate = dir / (std::string(moduleName) + ".ivm");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    // Also try current directory
    auto candidate = std::filesystem::current_path() / (std::string(moduleName) + ".ivm");
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) return candidate;
    return {};
}

}  // namespace ivy

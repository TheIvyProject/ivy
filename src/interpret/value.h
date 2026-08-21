#pragma once

// IvyInterpret — Runtime Value type.
// This module is intentionally self-contained: it only depends on
// the C++ standard library and <hir/hir.h> for ivy::Type. It has
// no dependency on parsing/, mir/, or codegen/.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ivy {
namespace interp {

struct Value;

// A struct instance: fields stored by name.
using StructFields = std::unordered_map<std::string, std::shared_ptr<Value>>;

// A heap-allocated cell (for pointer / reference semantics).
// shared_ptr so multiple aliases can point to the same cell.
using Cell = std::shared_ptr<Value>;

struct Value {
    // Void / unit — used for void-returning functions.
    struct Void {};
    // Integer (covers bool / all int widths).
    struct Int { long long v; };
    // Floating-point.
    struct Float { double v; };
    // String (copy-on-write is fine here — interpreter is not perf-critical).
    struct Str { std::string v; };
    // Struct aggregate.
    struct Struct { std::string typeName; StructFields fields; };
    // Pointer / reference — points to a heap Cell.
    struct Ptr { Cell cell; };

    std::variant<Void, Int, Float, Str, Struct, Ptr> data;

    // ---- Convenience constructors ----
    Value() : data(Void{}) {}
    explicit Value(long long v)   : data(Int{v}) {}
    explicit Value(bool v)        : data(Int{v ? 1LL : 0LL}) {}
    explicit Value(double v)      : data(Float{v}) {}
    explicit Value(std::string v) : data(Str{std::move(v)}) {}

    // Convenience predicates.
    bool isVoid()   const { return std::holds_alternative<Void>(data); }
    bool isInt()    const { return std::holds_alternative<Int>(data); }
    bool isFloat()  const { return std::holds_alternative<Float>(data); }
    bool isStr()    const { return std::holds_alternative<Str>(data); }
    bool isStruct() const { return std::holds_alternative<Struct>(data); }
    bool isPtr()    const { return std::holds_alternative<Ptr>(data); }

    long long      asInt()   const { return std::get<Int>(data).v; }
    double         asFloat() const { return std::get<Float>(data).v; }
    const std::string& asStr() const { return std::get<Str>(data).v; }
    Struct&        asStruct()  { return std::get<Struct>(data); }
    const Struct&  asStruct() const { return std::get<Struct>(data); }
    Cell&          asPtr()   { return std::get<Ptr>(data).cell; }
    const Cell&    asPtr()   const { return std::get<Ptr>(data).cell; }

    // Return a human-readable representation (for REPL / --run output).
    std::string toString() const {
        if (isVoid())   return "(void)";
        if (isInt())    return std::to_string(asInt());
        if (isFloat())  return std::to_string(asFloat());
        if (isStr())    return asStr();
        if (isPtr())    return asPtr() ? asPtr()->toString() : "(null)";
        if (isStruct()) {
            std::string out = asStruct().typeName + "{";
            bool first = true;
            for (const auto& [k, v] : asStruct().fields) {
                if (!first) out += ", ";
                out += k + "=" + (v ? v->toString() : "(null)");
                first = false;
            }
            return out + "}";
        }
        return "?";
    }
};

// Helper: make a heap Cell holding a copy of v.
inline Cell makeCell(Value v) {
    return std::make_shared<Value>(std::move(v));
}

}  // namespace interp
}  // namespace ivy

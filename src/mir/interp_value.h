#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ivy {
namespace mir {

// Provenance — tracks the origin of a pointer (inspired by Miri).
// Pointers are not just integers; they carry origin metadata for safety checks.
struct Provenance {
    enum Kind { None, Local, Static, Heap, Param } kind = None;
    uint32_t allocId = 0;       // ID of the allocation (for Heap/Local)
    std::string_view ltName;   // Lifetime name (for Param, from MIR Lifetime)
};

// Forward declaration for Cell (mutual dependency with Value).
struct Value;

// Cell = heap-allocated, shared-ownership value slot.
// Used for pointer/reference aliasing: multiple names can share one Cell.
using Cell = std::shared_ptr<Value>;

// Runtime value — hybrid representation (inspired by Miri's Operand).
struct Value {
    enum Kind { Void, Int, Float, Ptr, Struct, Str } kind = Void;
    long long i = 0;
    double f = 0.0;

    // Pointer value: a cell + provenance for safety tracking.
    struct PtrVal {
        Cell cell;              // shared_ptr<Value> — the pointee
        Provenance prov;        // origin tracking
        bool isNull = true;     // nullptr flag
    } ptr;

    // Struct value: type name + named fields (each field is a Cell for aliasing).
    struct StructVal {
        std::string typeName;
        std::unordered_map<std::string, Cell> fields;
    } strct;

    // String value: stores the decoded bytes of a string literal.
    // Lifetime is always Static (string literals live forever).
    std::string str;

    // Queries
    bool isVoid()   const { return kind == Void; }
    bool isInt()    const { return kind == Int; }
    bool isFloat()  const { return kind == Float; }
    bool isPtr()    const { return kind == Ptr; }
    bool isStruct() const { return kind == Struct; }
    bool isStr()    const { return kind == Str; }

    long long asInt() const { return i; }
    double asFloat() const { return f; }
    const std::string& asStr() const { return str; }

    std::string toString() const;
};

// Helper: create a heap cell from a Value.
inline Cell makeCell(Value v) {
    return std::make_shared<Value>(std::move(v));
}

// Helper: create a null pointer Value.
inline Value makeNullPtr() {
    Value v;
    v.kind = Value::Ptr;
    v.ptr.isNull = true;
    return v;
}

// Helper: create a pointer Value to a cell.
inline Value makePtr(Cell cell, Provenance prov = {}) {
    Value v;
    v.kind = Value::Ptr;
    v.ptr.cell = std::move(cell);
    v.ptr.prov = std::move(prov);
    v.ptr.isNull = false;
    return v;
}

// Helper: create an Int Value.
inline Value makeInt(long long i) {
    Value v;
    v.kind = Value::Int;
    v.i = i;
    return v;
}

// Helper: create a Float Value.
inline Value makeFloat(double f) {
    Value v;
    v.kind = Value::Float;
    v.f = f;
    return v;
}

// Helper: create a Str Value (string literal, static lifetime).
inline Value makeStr(std::string s) {
    Value v;
    v.kind = Value::Str;
    v.str = std::move(s);
    return v;
}

// Helper: create a Void Value.
inline Value makeVoid() {
    Value v;
    v.kind = Value::Void;
    return v;
}

}  // namespace mir
}  // namespace ivy

#pragma once

#include <string_view>
#include <vector>

#include "mir/interp_value.h"

namespace ivy {
namespace mir {

class Memory;

// Machine — safety policy hook (inspired by Miri's Machine trait).
// The core interpreter stays dumb; safety policy is injected via these hooks.
// Start with NoOpMachine, add checks incrementally (bounds, init, provenance).
class Machine {
public:
    virtual ~Machine() = default;

    // Called on EVERY memory access (load/store through a pointer).
    virtual void beforeMemoryAccess(const Value::PtrVal& /*ptr*/, bool /*isWrite*/,
                                     const Memory& /*mem*/) {}

    // Called on pointer creation (&x, new, etc.).
    virtual void onPointerCreate(const Value::PtrVal& /*ptr*/) {}

    // Called on function call.
    virtual void onCall(std::string_view /*fnName*/,
                        const std::vector<Value>& /*args*/) {}

    // Called on return — check dangling.
    virtual void onReturn(const Value& /*retVal*/, std::string_view /*retLt*/) {}
};

// NoOpMachine — plain interpreter, no safety checks.
// Used as the default when safety is not required (e.g. fast REPL).
class NoOpMachine : public Machine {};

}  // namespace mir
}  // namespace ivy

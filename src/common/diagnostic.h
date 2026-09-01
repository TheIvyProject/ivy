#pragma once

#include <cstdint>
#include <string>

namespace ivy {

struct Diagnostic {
    std::uint32_t line;  // 1-based
    std::uint32_t col;   // 1-based, in bytes
    std::string message;
    // 8.6: When true, this diagnostic is a warning (not a fatal error).
    // #warning emits warnings; #error emits errors. Other pipeline stages
    // always emit errors (isWarning == false, the default).
    bool isWarning = false;
};

}  // namespace ivy
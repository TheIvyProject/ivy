// 9.2: Module interface file (.ivm) writer and reader.
//
// An .ivm file is a text-based binary module interface (BMI) format
// similar to Clang's .pcm files. It stores the exported declarations
// of a module so that importers can resolve names without re-parsing
// the module source.
//
// Format:
//   line 1: "IVYMOD\x01" (magic + version)
//   line 2: "module <name>"
//   Then sections:
//     "func <mangled-name> <return-type> <param-count> <param-type> ..."
//     "struct <name> <field-count> <field-type>:<field-name> ..."
//     "enum <name> <is-scoped> <underlying-type> <count> <name>=<value> ..."
//     "using <name> <target-type>"
//     "concept <name> <param-name> <req-count> <req> ..."
//
// The format is line-oriented and human-readable for easy debugging.

#pragma once

#include "parsing/ast.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ivy {

// Write a .ivm module interface file from an AST TranslationUnit.
// Only declarations marked `isExported=true` are written.
// Returns true on success.
bool writeModuleInterface(const TranslationUnit& tu,
                         const std::filesystem::path& outputPath);

// Read a .ivm module interface file and merge its declarations into
// the given TranslationUnit. This is called when processing `import`
// declarations — the imported module's exports become available as
// declarations (without bodies) in the importing TU.
// Returns true on success.
bool readModuleInterface(TranslationUnit& tu,
                         const std::filesystem::path& ivmPath);

// Resolve a module name to a .ivm file path by searching the given
// directories. Returns empty path if not found.
std::filesystem::path resolveModulePath(std::string_view moduleName,
    const std::vector<std::filesystem::path>& searchDirs);

}  // namespace ivy

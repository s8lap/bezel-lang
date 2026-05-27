#include "codegen/CodeGen.hpp"

namespace bezel {

CodeGen::CodeGen(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

bool CodeGen::emit(ast::Node& unit) {
    (void)unit;
    // No backend selected yet — see the open decision in CodeGen.hpp.
    return !diagnostics_.hasErrors();
}

} // namespace bezel

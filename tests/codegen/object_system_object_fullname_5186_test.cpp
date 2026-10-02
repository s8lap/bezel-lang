// Regression tests for issue #5186 (spec §8.1, v0.311; stdlib §2.13).
//
// WHAT THIS PINS:
//
//   * `Object`'s module is `System`; `System.Object` is its qualified name
//     per §8.1. The `K::Primitive` arm of `spec85TypeRefQualifiedName`
//     renders `"System.Object"` (instead of the bare `"Object"` of rule
//     2's keyword-aliased case), so a generic INSTANTIATION with `Object`
//     as an argument renders `…<System.Object>` under FullName.
//   * `System.Object` resolves in type position (`var o: System.Object := …`)
//     and in chain-head position (`System.Object.ReferenceEquals(…)`).
//
// SCOPE NOTE. The issue's headline user-visible behavior —
// `Console.WriteLine(new Object())` printing `"System.Object"` — is not
// pinned here: `new Object()` itself doesn't lower today
// (`LlvmBackend_Exprs2.cpp:6107`'s `lowerNew` rejects `Kind::Primitive`,
// a separate pre-existing gap tracked independently). The §8.5 default
// path (`lowerObjectToString` slot-3 thunk + qualified-name rendering)
// IS pinned by `builtin_generic_arg_renders_system_object` (Object as
// a `FullName` type-argument). The chain-head resolution fix is pinned
// by `system_object_chain_head` and the resolution-row additions to
// `BuiltinRegistry`'s `kBuiltinClassModulePaths` (and the snapshot
// update in `tests/support/builtin_registry_snapshot_test.cpp`).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// runAotOut returns the program's stdout text (a single trailing line),
// stripped of the newline. expectations are the bare rendered string.
std::string out(const std::string& prog) { return runAotOut(prog); }

}  // namespace

// ===========================================================================
// stdlib §2.13 rule 3 — a generic INSTANTIATION with `Object` as an argument
// renders "System.Object" under FullName.
//
// The builtin class-typed receiver branch builds the qualified name from
// `builtinClassModulePathOf + arn` and then renders each generic argument
// through `spec85TypeRefQualifiedName`. Pre-fix: the argument rendered as
// `Object` (the bare `primitiveBuiltinSpelling`); post-fix: as
// `System.Object` (the §2.13 rule 1 qualified form). The full rendering
// for `Array<Object>` is therefore
// `System.Collections.Generic.Array<System.Object>`.
// ===========================================================================

BEZEL_TEST(object_system_object_fullname_5186, builtin_generic_arg_renders_system_object) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "class C public static method Main(): int "
            "  var a := new Array<Object>(1); "
            "  Console.WriteLine(a.ToString()); "
            "  return 0; "
            "end Main; end C;"),
        std::string("System.Collections.Generic.Array<System.Object>"));
}

// ===========================================================================
// §8.1 — `System.Object` resolves in type position. A direct `o = null`
// comparison prints via the `Console.WriteLine(bool)` overload
// (renders "True"/"False"), pinning the type-position fix without
// depending on a separate `bool.ToString()` lowering (bool's static
// surface is a closed set with no `ToString` — the §3.4 reject would
// fire on `(<bool-expr>).ToString()` regardless of the receiver's
// origin) or on `new Object()` (a separate pre-existing lowering gap
// in `LlvmBackend_Exprs2.cpp:6107`'s `lowerNew`, which rejects
// `Kind::Primitive`).
// ===========================================================================

BEZEL_TEST(object_system_object_fullname_5186, system_object_type_position) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "class C public static method Main(): int "
            "  var o: System.Object := 42; "
            "  Console.WriteLine(o = null); "
            "  return 0; "
            "end Main; end C;"),
        std::string("False"));
}

// ===========================================================================
// §8.1 — `System.Object` resolves in chain-head position. `ReferenceEquals`
// is a static method on `Object` and is reachable both as
// `Object.ReferenceEquals` (the bare-name dispatch path) and as
// `System.Object.ReferenceEquals` (the qualified-chain-head path my fix
// enables). The WriteLine is dispatched through Console.WriteLine's bool
// overload, which renders "True"/"False" — exercising the §8.5 path
// without depending on a separate `bool.ToString()` lowering.
//
// The chain-head form works WITHOUT `import System;` — `System` is a
// §2.7 rule 1 pseudo-module prefix for `Object` / `Type` per §8.1, not
// a real module. `resolveMemberAccess`'s chain-head stamp sets the
// inner `System.Object`'s exprType to `PrimitiveType::Object`, then
// does NOT return early (the §3.4 primitive-static dispatch path
// resolves the outer `ReferenceEquals` from the now-typed inner).
// ===========================================================================

BEZEL_TEST(object_system_object_fullname_5186, system_object_chain_head) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "class C public static method Main(): int "
            "  Console.WriteLine(System.Object.ReferenceEquals(null, null)); "
            "  return 0; "
            "end Main; end C;"),
        std::string("True"));
}
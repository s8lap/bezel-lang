// fix #1599 / §26.4/§26.5: an extension target that is a NESTED generic
// instantiation had its OWN head name wrongly treated as a free type
// parameter, e.g. `extension IEnumerable<Array<byte>>` — the inner `Array`
// head is a CONCRETE type per §26.4 ("a concrete instantiation is written
// with keyword/primitive types, qualified names, or generic instantiations
// (`extension IEnumerable<int>`, `extension IEnumerable<Array<byte>>`)"), not
// a free type parameter.
//
// Root cause: TypeResolution_Names.cpp's Resolver::resolveTopDecl handling of
// ExtensionDecl (the §26.4 implicit-type-parameter binding pass) walked only
// the extension target's TOP-LEVEL genericArgs, and for each arg tested only
// `kind == Named && name->parts.size() == 1 && !nullable` — without checking
// whether that arg carried ITS OWN nested genericArgs. `Array<byte>` passes
// that same test (`Array` is a bare, non-nullable, single-segment name), so
// its head `Array` was bound as a fresh block-level type parameter (like a
// bare `T` would be) and the `<byte>` argument was silently discarded. The
// extension target then spuriously unified with ANY `IEnumerable<X>`
// receiver — e.g. `Array<int>` — instead of being restricted to receivers
// whose element type is exactly `Array<byte>`.
//
// Fix: the binding walk (now Resolver::bindExtensionTargetTypeParams,
// TypeResolution_Names.cpp) recurses into a generic arg's OWN genericArgs
// when non-empty — such an arg is a concrete generic-instantiation head, per
// §26.4's "any nesting depth" rule (v0.183: "`extension IEnumerable<Array<T>>`
// binds T") — rather than collapsing it into an opaque type-param marker.
// Only a bare identifier with NO generic args of its own binds as a free
// type parameter.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runJit(program).ran;
}
}  // namespace

// §26.4 spec_1599_1 (the repro) — `extension IEnumerable<Array<byte>>` is a
// CONCRETE instantiation target; it must NOT match an `Array<int>` receiver
// (element type `int` != `byte`). Was: compiled and dispatched (the `Array`
// head was wrongly bound as a free type parameter, so any element type
// unified). Should be: a "no member" compile error.
BEZEL_TEST(extension_nested_generic_target_1599, concrete_nested_head_does_not_over_match) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "extension IEnumerable<Array<byte>> "
        "  public method Tag(): int => 42; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Tag(); "
        "end Main; end App;"));
}

// §26.4 spec_1599_2 — the same target DOES match a receiver whose element
// type is exactly `Array<byte>`, confirming the concrete instantiation still
// resolves and dispatches correctly (not merely rejected-everywhere).
BEZEL_TEST(extension_nested_generic_target_1599, concrete_nested_head_matches_exact_instantiation) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<Array<byte>> "
        "  public method Tag(): int => 42; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<Array<byte>> := []; "
        "  return xs.Tag(); "
        "end Main; end App;"), 42);
}

// §26.4 spec_1599_3 — a genuinely free (non-nested) type-parameter target,
// `extension IEnumerable<T>`, still binds T and matches any element type
// (regression guard: the fix must not disturb the pre-existing, correct
// no-nesting case covered by #1276's tests).
BEZEL_TEST(extension_nested_generic_target_1599, bare_free_param_target_still_matches) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<T> "
        "  public method Tag(): int => 1; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Tag(); "
        "end Main; end App;"), 1);
}

// §26.4 spec_1599_4 (v0.183 "any nesting depth") — `extension
// IEnumerable<Array<T>>` binds the NESTED bare identifier `T` as a free type
// parameter (its enclosing head `Array` is still the concrete instantiation
// marker, not itself a parameter). It matches `Array<Array<int>>`, binding
// T = int.
BEZEL_TEST(extension_nested_generic_target_1599, nested_bare_identifier_binds_as_free_param) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<Array<T>> "
        "  public method Tag(): int => 7; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<Array<int>> := []; "
        "  return xs.Tag(); "
        "end Main; end App;"), 7);
}

// §26.4 spec_1599_5 — the nested-free-param target from spec_1599_4 must NOT
// match a receiver that isn't itself nested inside an `Array<...>` (a plain
// `Array<int>` implements `IEnumerable<int>`, not `IEnumerable<Array<T>>` for
// any T).
BEZEL_TEST(extension_nested_generic_target_1599, nested_bare_identifier_does_not_over_match_flat_receiver) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "extension IEnumerable<Array<T>> "
        "  public method Tag(): int => 7; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Tag(); "
        "end Main; end App;"));
}

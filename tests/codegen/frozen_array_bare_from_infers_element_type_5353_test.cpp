// Regression tests for issue #5353 — bare `FrozenArray.From(src)` is untyped.
//
// Mechanism: the #3249 arm reads `typeArgs` off the callee target's own
// `typeArgs`, not the call-site's. For `FrozenArray<int>.From(...)` the
// target is `FrozenArray<int>` with `typeArgs=[int]` baked in. For the
// bare form the target is `FrozenArray` with `typeArgs=[]`, so the
// element type is unknown and sema issues "cannot infer the type
// argument".
//
// The fix: when `typeArgs` is empty (bare form), infer the type argument
// from the SOURCE argument's type. `FrozenArray.From(src: Array<int>)`
// yields `FrozenArray<int>` — the element type of the source Array is
// the FrozenArray's element type.
//
// Cite: stdlib §4.19 (v0.315).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Build a program whose body ends with `return <expr>;`.
int run(const std::string& body) {
    const std::string p =
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end Program;";
    auto r = runAot(p);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ═══════════ Primary bug: bare FrozenArray.From(...) compiles and runs ═══════
//
// ACCEPT row: bare spelling — `FrozenArray.From(src)` with no type argument,
// where `src` is a typed Array variable. This is the #5353 regression:
// pre-fix sema issues "cannot infer the type argument `T` of `From`'s
// call" and compilation fails. Post-fix: compiles cleanly and runs,
// producing the correct frozen contents.
BEZEL_TEST(frozen_array_bare_from_infers_element_type_5353,
           bare_from_typed_array_variable_accepts_and_runs_stdlib_4_19) {
    // Build a FrozenArray from a typed Array<int> variable using the bare
    // spelling, verify count and contents via indexing.
    const std::string body =
        "  var src: Array<int> := [10, 20, 30]; "
        "  var fa := FrozenArray.From(src); "
        "  var n := fa.Count(); "
        "  if n <> 3 then return 1; end; "
        "  if fa[0] <> 10 then return 2; end; "
        "  if fa[1] <> 20 then return 3; end; "
        "  if fa[2] <> 30 then return 4; end; "
        "  return 42; ";
    BEZEL_EXPECT_EQ(run(body), 42);
}

// CONTROL row: the explicit spelling `FrozenArray<int>.From(...)` was
// already working — this row proves the fix does not break the existing
// path and that the bare form is a genuine improvement, not a
// "everything became accepted" regression.
BEZEL_TEST(frozen_array_bare_from_infers_element_type_5353,
           explicit_from_typed_array_variable_still_works_control_stdlib_4_19) {
    const std::string body =
        "  var src: Array<int> := [10, 20, 30]; "
        "  var fa := FrozenArray<int>.From(src); "
        "  var n := fa.Count(); "
        "  if n <> 3 then return 1; end; "
        "  if fa[0] <> 10 then return 2; end; "
        "  if fa[1] <> 20 then return 3; end; "
        "  if fa[2] <> 30 then return 4; end; "
        "  return 42; ";
    BEZEL_EXPECT_EQ(run(body), 42);
}

// String element type — strings are the primary motivation for From(Array<T>)
// per stdlib §4.19.
BEZEL_TEST(frozen_array_bare_from_infers_element_type_5353,
           bare_from_typed_string_array_variable_accepts_stdlib_4_19) {
    const std::string body =
        "  var src: Array<string> := [\"hello\", \"world\"]; "
        "  var fa := FrozenArray.From(src); "
        "  var n := fa.Count(); "
        "  if n <> 2 then return 1; end; "
        "  if fa[0] <> \"hello\" then return 2; end; "
        "  if fa[1] <> \"world\" then return 3; end; "
        "  return 42; ";
    BEZEL_EXPECT_EQ(run(body), 42);
}

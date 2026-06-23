// fix #714 / §26.5: calling a user-defined generic extension method on a
// concrete receiver binds the receiver's type parameter and dispatches
// correctly through the interface-ancestor chain.
//
// Repro: `extension IEnumerable<T>` with `method MyCount(): int` called on
// `Array<int>` (which implements IEnumerable<int>) returned garbage (silent
// miscompile) because sema's lookupExtensionMember did not:
//   (a) scan the target's generic args for implicit free type variables (T in
//       `IEnumerable<T>` without a `where` clause has empty genericConstraints),
//   (b) walk the builtin receiver's interface-ancestor chain to reach
//       IEnumerable<int> from Array<int>, or
//   (c) record the T→int binding for codegen to set up genericSubst_.
//
// Derived from spec §26.5 (generic extension matching and dispatch).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §26.5 spec_714_1 — a `extension IEnumerable<T>` method with `int` return
// type (T not in signature) called on `Array<int>`. The method body counts
// elements via for-in and returns the count. Was: -999 (miscompile / not found).
// Should be: 3.
BEZEL_TEST(generic_extension_714, ienumerable_extension_on_array_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<T> "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end; "
        "public static class P public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.MyCount(); "
        "end Main; end P;"), 3);
}

// §26.5 spec_714_2 — same extension method called on an empty Array<int>.
// Should be: 0.
BEZEL_TEST(generic_extension_714, ienumerable_extension_empty_array) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<T> "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  return xs.MyCount(); "
        "end Main; end P;"), 0);
}

// §26.5 spec_714_3 — calling the same extension method on a different
// concrete instantiation (Array<int> with 4 elements). Should be: 4.
BEZEL_TEST(generic_extension_714, ienumerable_extension_four_elements) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<T> "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end; "
        "public static class P public static method Main(): int "
        "  var xs: Array<int> := [10, 20, 30, 40]; "
        "  return xs.MyCount(); "
        "end Main; end P;"), 4);
}

// §26.5 spec_714_4 — extension method with `where T: class` clause on
// IEnumerable<T> (explicit constraint) called on Array<int>. The where clause
// populates genericConstraints so this tests the original freeVars path (not
// the new scanFreeVars path). Should be: 3.
BEZEL_TEST(generic_extension_714, ienumerable_extension_where_clause_on_int_array) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension IEnumerable<T> where T: notnull "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end; "
        "public static class P public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.MyCount(); "
        "end Main; end P;"), 3);
}

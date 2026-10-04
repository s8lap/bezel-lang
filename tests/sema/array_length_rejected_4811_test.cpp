// fix #4811 (stdlib §4.4 Count-vs-Length convention p.74, v0.315):
// `Array<T>` and `OwningArray<T>` declare only `Count` per the Count-vs-Length
// convention: "the resizable Array<T> says Count where .NET's fixed array says
// Length". The implementation (added by #4766) typed `Array<T>.Length` as an
// alias for `Count`, which is an undocumented impl/stdlib divergence.
//
// After the fix, `Array<T>.Length` is a "no member" reject. The fix removes
// Array/OwningArray from kBuiltinIntLengthPropertyHeads
// (common/BuiltinRegistry.cpp).
//
// Rows:
//   R1  Array<int>.Length — direct access on Array<T>, rejected as "no member
//       Length". Pins that the fix is not a no-op.
//   R2  Array<string>.Length on an array literal — same reject.
//   R3  OwningArray<int>.Length — OwningArray<T> is also Count-only per §4.5.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — Array<int>.Length is rejected.
BEZEL_TEST(sema_array_length_rejected_4811,
           array_int_length_rejected_no_member_4_4) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := new Array<int>(); a.Add(1); "
        "  return a.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R2 — Array<string>.Length from a literal is rejected.
BEZEL_TEST(sema_array_length_rejected_4811,
           array_string_length_literal_rejected_no_member_4_4) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var xs: Array<string> := [\"a\", \"b\"]; "
        "  return xs.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R3 — OwningArray<Widget>.Length is rejected (Count-only per §4.5).
BEZEL_TEST(sema_array_length_rejected_4811,
           owning_array_widget_length_rejected_no_member_4_5) {
    auto r = runAot(
        "module M; class Widget public constructor() end constructor; end Widget; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Widget>(); a.Add(new Widget()); "
        "  return a.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

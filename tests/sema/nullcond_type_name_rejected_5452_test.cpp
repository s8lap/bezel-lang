// fix #5452 (§7.5, v0.315):
// `Console?.WriteLine(s)` — null-conditional access through a class type name
// (a static receiver, not a nullable receiver value) compiled with no diagnostic,
// ran to a correct exit code, and silently skipped the call (0 bytes to stdout).
// This is silent wrong code: a dropped runtime side effect behind a clean
// compile and a correct exit code.
//
// Spec §7.5 defines `x?.M` over a receiver VALUE: "returns null if x is null;
// otherwise it performs the access". A class type name is not a receiver value
// and is never null, so the short-circuit premise never holds for it — the
// `?.` form over a type name is undefined by §7.5 and is now rejected.
//
// Rows:
//   R1  `Console?.WriteLine(s)` on a non-null String? argument — rejected.
//   R2  Control: `Console.WriteLine(s)` (non-?. form) still compiles and
//       produces output (6 bytes for "hello\n").
//   R3  Control: `Console?.WriteLine` on a NULL nullable receiver still
//       compiles and skips the call (the valid `?.` short-circuit path).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — `TypeName?.member` is rejected as invalid application of `?.`.
BEZEL_TEST(sema_nullcond_type_name_rejected_5452,
           console_type_name_nullcond_rejected_7_5) {
    auto r = runAot(
        "module T; class C public static method Main(): int "
        "var s: String? := \"hello\"; "
        "Console?.WriteLine(s); "
        "return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── R2 — Control: the non-?. form `Console.WriteLine(s)` still compiles
//         and produces output.
BEZEL_TEST(sema_nullcond_type_name_rejected_5452,
           console_static_call_still_works_7_5_control) {
    auto r = runAot(
        "module T; class C public static method Main(): int "
        "var s: String? := \"hello\"; "
        "Console.WriteLine(s); "
        "return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_EQ(r.stdoutText, "hello\n");
}

// ── R3 — Control: `?.` on a genuinely nullable CLASS receiver (null value)
//         still compiles and skips the call (the valid §7.5 short-circuit).
//         Uses Widget?.Dispose() — a known-valid `?.` on a nullable class.
BEZEL_TEST(sema_nullcond_type_name_rejected_5452,
           null_class_receiver_nullcond_still_skips_7_5_control) {
    auto r = runAot(
        "module T; class Widget public constructor() end constructor; end Widget; "
        "class C public static method Main(): int "
        "var w: Widget? := null; "
        "w?.Dispose(); "
        "return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

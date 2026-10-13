// Spec-derived regression test for issue #6077.
//
// §30.11 (v0.338) / stdlib §6.6 declare the exact argument lists of the
// TaskCompletionSource family:
//
//     TaskCompletionSource<T>:          SetResult(result: T)
//                                       TrySetResult(result: T): bool
//                                       SetCancelled()
//                                       TrySetCancelled(): bool
//     OwningTaskCompletionSource<T>:    SetResult(owned result: T)
//                                       TrySetResult(owned result: T): bool
//                                       SetCancelled() / TrySetCancelled(): bool
//     TaskCompletionSource (non-gen.):  SetResult() / TrySetResult(): bool
//                                       SetCancelled() / TrySetCancelled(): bool
//
// No cancel form declares a parameter, so any argument to SetCancelled or
// TrySetCancelled is an arity error. The generic result forms take exactly one
// argument. The sema arm (ExprTypecheck_Calls.cpp, the TaskCompletionSource
// family block in typeCall) types these calls and checked only the non-generic
// result forms (#6064), so these shapes were silently accepted.
//
// Rows are runAot oracles. Reject rows assert the located arity diagnostic;
// accept rows run the program and assert stdout. The generic zero-argument
// SetResult() keeps the codegen no-dispatch rejection, and a row pins that it
// is not double-reported by a second sema diagnostic.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// REJECT — non-generic SetCancelled with one argument (no parameter declared).
BEZEL_TEST(sema_tcs_argcount_6077,
           nongeneric_setcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource();\n"
        "    t.SetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetCancelled` has no overload that "
                                      "accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — non-generic TrySetCancelled with one argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           nongeneric_trysetcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource();\n"
        "    var b: bool := t.TrySetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:20: `TrySetCancelled` has no overload "
                                      "that accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — generic TaskCompletionSource<int>.SetResult with two arguments.
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_setresult_two_args_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    t.SetResult(1, 2);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetResult` has no overload that "
                                      "accepts 2 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — generic TaskCompletionSource<int>.TrySetResult with no argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_trysetresult_zero_args_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    var b: bool := t.TrySetResult();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:20: `TrySetResult` has no overload "
                                      "that accepts 0 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — OwningTaskCompletionSource<Res>.TrySetResult with a second argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           owning_trysetresult_two_args_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new OwningTaskCompletionSource<Res>();\n"
        "    var b: bool := t.TrySetResult(new Res(), 1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"
        "public class Res\n"
        "end Res;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:20: `TrySetResult` has no overload "
                                      "that accepts 2 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT-NOT-DOUBLE-REPORTED — generic zero-argument SetResult() keeps its
// existing codegen no-dispatch rejection. The sema arity text must not appear
// alongside it.
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_setresult_zero_args_keeps_existing_diagnostic_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    t.SetResult();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`SetResult` on built-in type "
                                      "`TaskCompletionSource` produced no value"));
    BEZEL_EXPECT_TRUE(!diagContains(r, "has no overload that accepts"));
}

// ACCEPT (control) — the spec-form zero-argument non-generic cancel calls stay
// accepted by sema. Output is asserted; the cancel is not awaited.
BEZEL_TEST(sema_tcs_argcount_6077,
           nongeneric_cancel_zero_args_accepted_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource();\n"
        "    t.SetCancelled();\n"
        "    var b: bool := t.TrySetCancelled();\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

// ACCEPT (control) — the one-argument generic result forms, on both the
// TaskCompletionSource<T> and the owning twin, stay accepted.
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_and_owning_one_arg_result_forms_accepted_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    var b: bool := t.TrySetResult(6);\n"
        "    using var o := new OwningTaskCompletionSource<Res>();\n"
        "    o.SetResult(new Res());\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"
        "public class Res\n"
        "end Res;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

// REJECT — generic TaskCompletionSource<int>.SetCancelled with one argument
// (stdlib §6.6 declares SetCancelled() with no parameter on every form).
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_setcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    t.SetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetCancelled` has no overload that "
                                      "accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — generic TaskCompletionSource<int>.TrySetCancelled with one argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_trysetcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    var b: bool := t.TrySetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:20: `TrySetCancelled` has no overload "
                                      "that accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — OwningTaskCompletionSource<Res>.SetCancelled with one argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           owning_setcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new OwningTaskCompletionSource<Res>();\n"
        "    t.SetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"
        "public class Res\n"
        "end Res;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetCancelled` has no overload that "
                                      "accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — OwningTaskCompletionSource<Res>.TrySetCancelled with one argument.
BEZEL_TEST(sema_tcs_argcount_6077,
           owning_trysetcancelled_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new OwningTaskCompletionSource<Res>();\n"
        "    var b: bool := t.TrySetCancelled(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"
        "public class Res\n"
        "end Res;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:20: `TrySetCancelled` has no overload "
                                      "that accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// ACCEPT (control) — the zero-argument cancel forms stay accepted on the
// generic and owning receivers (the non-generic zero-argument form is covered
// by nongeneric_cancel_zero_args_accepted_spec_30_11 above).
BEZEL_TEST(sema_tcs_argcount_6077,
           generic_and_owning_cancel_zero_args_accepted_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var t := new TaskCompletionSource<int>();\n"
        "    t.SetCancelled();\n"
        "    var b: bool := t.TrySetCancelled();\n"
        "    using var o := new OwningTaskCompletionSource<Res>();\n"
        "    o.SetCancelled();\n"
        "    var c: bool := o.TrySetCancelled();\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"
        "public class Res\n"
        "end Res;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

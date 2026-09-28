// fix #5019 — codegen follow-up to #4978 (§27.6, v0.307): method-reference
// thunk emission reads the chosen overload's MethodSymbol, not the bound
// (first-declared) MethodSymbol.
//
// THE PROBLEM. The §27.6 formation-site overload selector (#4978) picks the
// overload whose signature unifies with the target function type under §5.1,
// and the SEMANTIC compile result is green — the chosen overload's Function
// TypeRef is what the formation-site check runs against. The runtime thunk
// emitted by `LlvmBackend_Exprs.cpp`'s CG60 arm, however, reads
// `ctx_.exprSymbol(e)` and binds the FIRST-declared overload (`lookupMember`'s
// first-match BFS), not the chosen one. For a target like `(int) -> ()` over
// `H.On(string)` and `H.On(int)`, the bound symbol is `On(string)`; the
// chosen symbol is `On(int)`. The emitted thunk calls `On(string)` with the
// (int) argument, and the binary crashes at runtime (SIGSEGV on the wrong-
// typed call). This file pins the codegen half: the same probe that #4978
// pinned at the sema level now has to produce the runtime result.
//
// FAIL-WITHOUT (verified on `e4f4434e`, pre-fix base):
//   * all six rows crash with SIGSEGV — the chosen overload's body is not
//     the body the thunk dispatches to.
//
// AXIS GRID:
//   1. Verbatim probe from the issue body — `H.On(string)`, `H.On(int)`,
//      target `(int) -> ()`, call `f(42)`. Pins the bug.
//   2. Mirror case — string target, same overloads. Pins that exact-preferred
//      ranking survives codegen.
//   3. Third-overload class — `On(string)`, `On(int)`, `On(double)`, target
//      `(double) -> ()`. Pins the same defect on a 3-overload class.
//   4. Middle overload chosen — `(string, int, double)` declarations, target
//      `(int) -> ()`, chosen = middle. Pins that the chosen overload is the
//      TARGET-RESOLVED one, not the first declared (which would be `string`).
//   5. Instance method form — same shape, on an instance method. Pins that
//      the fix is shape-agnostic.
//
// per §27.6 (issue #5019, spec v0.307).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Run a whole module source. Returns the captured stdout and exit code
// (the JIT-side `runAot` returns a JitResult with `ran`/`exitCode`/
// `stdoutText`). Fails the assertion if the program did not run.
struct RunOut { bool ran; int exitCode; std::string stdoutText; };
RunOut runMod(const std::string& src) {
    JitResult r = runAot(src);
    return {r.ran, r.exitCode, r.stdoutText};
}

}  // namespace

// =====================================================================
// 1. Issue #5019 verbatim probe — `H.On(string)` and `H.On(int)` declared,
//    target `(int) -> ()`, chosen = `On(int)`. Pre-fix: SIGSEGV (the thunk
//    calls `On(string)` with an int argument). Post-fix: prints "int 42\n".
// per §27.6 (issue #5019, spec v0.307)
// =====================================================================
BEZEL_TEST(codegen_method_ref_target_typed_5019,
           static_overloaded_target_int_int_overload_chosen_spec_27_6) {
    auto r = runMod(
        "module M; "
        "public class H "
        "  public static method On(s: string): () => "
        "    Console.WriteLine($\"str {s}\"); "
        "  public static method On(i: int): () => "
        "    Console.WriteLine($\"int {i}\"); "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var f: (int) -> () := H.On; "
        "    f(42); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "int 42\n");
}

// =====================================================================
// 2. Mirror case — string target, same overloads. Pins that exact-preferred
//    ranking survives codegen (chosen = `On(string)`, not the first-
//    declared `On(string)` by accident — same shape, different reasoning).
// per §27.6 (issue #5019, spec v0.307)
// =====================================================================
BEZEL_TEST(codegen_method_ref_target_typed_5019,
           static_overloaded_target_string_string_overload_chosen_spec_27_6) {
    auto r = runMod(
        "module M; "
        "public class H "
        "  public static method On(s: string): () => "
        "    Console.WriteLine($\"str {s}\"); "
        "  public static method On(i: int): () => "
        "    Console.WriteLine($\"int {i}\"); "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var f: (string) -> () := H.On; "
        "    f(\"x\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "str x\n");
}

// =====================================================================
// 3. Third-overload class — `On(string)`, `On(int)`, `On(double)` declared,
//    target `(double) -> ()`. Pins that the chosen overload is the one
//    whose signature unifies with the target, on a 3-overload class.
// per §27.6 (issue #5019, spec v0.307)
// =====================================================================
BEZEL_TEST(codegen_method_ref_target_typed_5019,
           three_overloads_target_double_double_overload_chosen_spec_27_6) {
    auto r = runMod(
        "module M; "
        "public class H "
        "  public static method On(s: string): () => "
        "    Console.WriteLine($\"str {s}\"); "
        "  public static method On(i: int): () => "
        "    Console.WriteLine($\"int {i}\"); "
        "  public static method On(d: double): () => "
        "    Console.WriteLine($\"dbl {d}\"); "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var f: (double) -> () := H.On; "
        "    f(3.14); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "dbl 3.14\n");
}

// =====================================================================
// 4. MIDDLE overload chosen — `(string, int, double)` declared (three
//    overloads), target `(int) -> ()`. Pre-fix the thunk dispatches to
//    `On(string)` (the first declared) and crashes; post-fix the thunk
//    dispatches to `On(int)` (the middle, target-resolved) and prints
//    "int 42\n". This pins that the chosen overload is the TARGET-RESOLVED
//    one, not the first declared — a strict-mode check that would fail
//    with a "bind the first declared overload" implementation even when the
//    first-declared happens to fit (the chosen is the MIDDLE one here).
// per §27.6 (issue #5019, spec v0.307)
// =====================================================================
BEZEL_TEST(codegen_method_ref_target_typed_5019,
           three_overloads_target_int_middle_overload_chosen_spec_27_6) {
    auto r = runMod(
        "module M; "
        "public class H "
        "  public static method On(s: string): () => "
        "    Console.WriteLine($\"str {s}\"); "
        "  public static method On(i: int): () => "
        "    Console.WriteLine($\"int {i}\"); "
        "  public static method On(d: double): () => "
        "    Console.WriteLine($\"dbl {d}\"); "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var f: (int) -> () := H.On; "
        "    f(42); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "int 42\n");
}

// =====================================================================
// 5. Instance method form — same shape on an INSTANCE method (the receiver
//    is captured by the closure). Pins that the fix is shape-agnostic and
//    applies to non-static method refs as well as static ones. Pre-fix the
//    thunk dispatches `h.On(string)` and crashes; post-fix it dispatches
//    `h.On(int)`.
// per §27.6 (issue #5019, spec v0.307)
// =====================================================================
BEZEL_TEST(codegen_method_ref_target_typed_5019,
           instance_overloaded_target_int_int_overload_chosen_spec_27_6) {
    auto r = runMod(
        "module M; "
        "public class H "
        "  public method On(s: string): () => "
        "    Console.WriteLine($\"str {s}\"); "
        "  public method On(i: int): () => "
        "    Console.WriteLine($\"int {i}\"); "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var f: (int) -> () := h.On; "
        "    f(42); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "int 42\n");
}

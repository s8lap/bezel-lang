// Regression test for issue #9071 — a GENERIC ITERATOR that captures its `T`
// parameter, instantiated at a FUNCTION type, double-frees the argument
// closure (the non-generic twin is clean).
//
// THE DEFECT (§14.1 / §27.5 / §28.9, v0.359).
//   `public static method M<T>(p: T): IEnumerable<int>` — the parameter `p` is
//   captured by a lambda the iterator body calls. At the instantiation
//   `M<() -> string>` the parameter is a FUNCTION-typed value, so §28.9 makes it
//   runtime-managed: "a function value is runtime-managed … one reference per
//   co-owner", and §27.5's capture cell owns a reference to the captured value.
//
//   The iterator FRAME holds the parameter across every MoveNext (§29.3), so it
//   must take its OWN +1 on the closure cell and drop it at its Dispose — the
//   non-generic twin's arrangement (#1864). The frame asked the parameter's
//   DECLARED type, which for `p: T` is the bare type parameter, never the
//   substituted one; so at a FUNCTION-type instantiation the frame took no +1.
//
//   MEASURED on the base (`r30-c1` @ 346f3191b, `bezelc --sanitize=address`):
//     ERROR: AddressSanitizer: attempting double-free
//       #1 bz_closure_release   src/runtime/bezel_runtime_refcounting.cpp:920
//       #2 bz$lam$2$envdtor
//       #3 bz_closure_release   src/runtime/bezel_runtime_refcounting.cpp:896
//       #4 bz$iter$P.Program$M$MoveNext.2
//   The sequence (read off the emitted code): the caller mints the argument
//   closure (rc = 1) and the frame stores it WITHOUT a retain; the caller then
//   releases its transient argument reference at the call's end (rc 1 -> 0, the
//   cell is FREED); the body's capture retains the already-freed cell (an
//   atomic RMW, which ASan cannot instrument, so the use-after-free WRITE is
//   invisible); and the capture cell's envdtor releases it again -> the second
//   free.
//
// ORACLE. `runAotChecked` (AddressSanitizer over the generated module +
//   LeakSanitizer); every row loops 200 times inside the program, so a
//   per-iteration defect is amplified and cannot hide under the sanitizer's
//   noise.
//
// The fix (src/codegen/LlvmBackend_Iterators.cpp) reads the parameter's type
//   AT THE CURRENT SUBSTITUTION at all three frame sites — the construction
//   retain, the GetEnumerator clone's retain, and the Dispose release — so the
//   frame's +1 is taken (and dropped) exactly when the parameter is
//   function-typed AT THIS INSTANTIATION.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// =============================================================================
// RED on the base: the generic iterator at a FUNCTION-type instantiation.
// =============================================================================

// Issue #9071's own program, unchanged. The argument is a fresh TEMPORARY
// closure: the caller mints it (rc = 1) and releases it at the call's end.
// RED on the base: `ERROR: AddressSanitizer: attempting double-free`.
BEZEL_TEST(generic_iterator_captures_t_param_double_free_9071,
           issue_repro_generic_iterator_at_function_type_instantiation) {
    const std::string src =
        "module P; "
        "public class Program "
        "  public static method M<T>(p: T): IEnumerable<int> "
        "    var r := lambda(): int => do var y := p; return 1; end; "
        "    yield return r(); "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      for var n in Program.M<() -> string>(lambda(): string => \"k\") "
        "      do t := t + n; end; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

// =============================================================================
// GREEN control (a boundary row): the SAME generic iterator instantiated at a
// NON-function type. §28.9 makes only a FUNCTION-typed value runtime-managed
// with a reference count; an `int` parameter has no closure cell to retain, so
// the frame must take no +1 on it.
//
// This row is green on the base and on the fix, and it is RED under the
// plausible wrong fix this commit's body names: a fix that retains (and
// releases) EVERY captured parameter as though it were a closure cell would
// retain an `int`'s bits as a pointer.
// =============================================================================

BEZEL_TEST(generic_iterator_captures_t_param_double_free_9071,
           int_instantiation_takes_no_closure_reference) {
    const std::string src =
        "module P; "
        "public class Program "
        "  public static method M<T>(p: T): IEnumerable<int> "
        "    var r := lambda(): int => do var y := p; return 1; end; "
        "    yield return r(); "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      for var n in Program.M<int>(7) do t := t + n; end; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "200\n");
}

}  // namespace

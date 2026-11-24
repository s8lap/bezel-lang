// Regression test for issue #9118 — a generic lambda result typed by a bare
// type parameter took the +1 string mint only at a NON-NULLABLE `string`
// substitution, so an ESCAPING closure returning `T` at `T = string?`
// handed its caller a reference the caller then released: `refcount
// over-release (string)`, exit 134.
//
// THE DEFECT. A lambda's result carries a reference to its caller: the caller
// of a function value releases a `string` result exactly as it releases a
// `string?` one (the closure-call arm of `isPlusOneStringProducer`), so the
// lambda must MINT that +1 (a `bz_string_retain`) before handing the value
// over.
//
// Inside a generic instantiation, sema types the lambda's body ONCE, at the
// bare `T`: `M<T>(p: T): () -> T { return lambda(): T => p; }` has a lambda
// whose declared result is `T`. The decision whether that result takes the
// +1 mint is made at the CONCRETE substitution — `mintedBareTypeParamResult`
// (fix #8946 / #9007), the ONE predicate shared by the mint and by every
// consumer that must agree with it.
//
// That predicate declined a NULLABLE binding (`T = string?`), so the mint
// never fired for it: the lambda returned a +0 borrow while its caller —
// which sees the instantiated `string?` — classified the closure call as a
// +1 producer and RELEASED a reference the lambda never minted.
//
//   bezel: fatal: refcount over-release (string) — two releases for one
//   reference; terminating per §28.9        (exit 134)
//
// MEASURED on the base (r30-c1 @ f282cb74d), `bezelc --sanitize=address`, the
// issue's own program: `refcount over-release (string)`, exit 134, no output.
// The non-nullable `T = string` twin printed 1090, clean.
//
// THE SPEC.
//   §14.1 (v0.359): a generic declaration is instantiated at a concrete type
//   argument, and the body's types are the instantiation's.
//   §27.5 (v0.359): "Captured variables are heap-allocated in a closure
//   object" — the closure may outlive the frame that formed it, so the value
//   it hands back must be a reference the caller can hold.
//   §28.9 (v0.359): `string` is runtime-managed — "the type's lifetime is
//   managed by the runtime without user involvement". A `string?` is the same
//   runtime-managed reference with a null state, and the runtime's own
//   retain/release are null-safe, so the two forms need the SAME treatment
//   wherever a reference is taken or dropped.
//   §28.3 (v0.359): the caller of a function value that produces a string
//   owns the produced reference and releases it.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the defect cannot hide under the sanitizer's noise.
//
// ⚠ WHY THE STRINGS ARE BUILT AT RUNTIME. A constant-folded literal is an
// RC-SENTINEL on which the refcount calls are no-ops, so a literal-built probe
// passes whether or not the defect is present. Every program below builds its
// strings with interpolation (`$"fr-{j}"`), which allocates.
//
// ⚠ THE VALUES. "fr-0".."fr-199" are 4, 5 and 6 characters long:
// 10*4 + 90*5 + 100*6 = 1090.

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

// ROW 1 — the issue's literal repro. The generic method's parameter is typed
// by the bare type parameter `T`; the lambda returns it BY NAME, and the
// closure ESCAPES the call (it is invoked after `M` returns, so the
// capture cell outlives the frame that formed it).
//
// The instantiation is `T = string?` — the nullable form of the same
// runtime-managed reference.
//
// RED on the base: `refcount over-release (string)`, exit 134 — the caller's
// release of a reference the lambda never minted.
BEZEL_TEST(ownership_generic_return_site_nullable_string_substitution_9118,
           the_generic_return_site_mints_at_a_nullable_string_substitution) {
    const std::string src =
        "module P; public class Program "
        "  public static method M<T>(p: T): () -> T "
        "    return lambda(): T => p; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M<string?>($\"fr-{j}\"); "
        "      t := t + (f() ?? \"\").Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

// ROW 2 — the BLOCK-BODIED twin of row 1. A lambda has two body forms and
// they must agree on the mint: the expression-bodied form mints in
// `lowerLambda`, the block-bodied one in `emitReturn`, and both read the same
// classified result type. A fix that moved the decision to one form only would
// leave this row over-releasing.
//
// RED on the base: `refcount over-release (string)`, exit 134.
BEZEL_TEST(ownership_generic_return_site_nullable_string_substitution_9118,
           the_block_bodied_twin_mints_at_a_nullable_string_substitution) {
    const std::string src =
        "module P; public class Program "
        "  public static method M<T>(p: T): () -> T "
        "    return lambda(): T => do return p; end; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M<string?>($\"fr-{j}\"); "
        "      t := t + (f() ?? \"\").Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

// ROW 3 — the suite's BOUNDARY CONTROL: the NON-NULLABLE generic twin of
// row 1, the form the issue reports as already correct. It is green on the
// base and pins that the fix did not change the already-correct form's
// refcount balance — the plausible wrong fix it catches is a retain taken
// twice (a +1 per call, which LeakSanitizer sees), or the mint widened to a
// binding that is not a runtime-managed reference at all.
//
// A corpus case cannot express this check (the corpus runner has no ASan leg,
// so it cannot count the references); the plausible wrong fix it catches is
// named in the commit body.
BEZEL_TEST(ownership_generic_return_site_nullable_string_substitution_9118,
           the_non_nullable_generic_twin_stays_balanced) {
    const std::string src =
        "module P; public class Program "
        "  public static method M<T>(p: T): () -> T "
        "    return lambda(): T => p; "
        "  end M; "
        "  public static method Run(): int "
        "    var t := 0; "
        "    for var j in 0..<200 do "
        "      var f := Program.M<string>($\"fr-{j}\"); "
        "      t := t + f().Length; "
        "    end; "
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Program.Run()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAotChecked(src), 0, "1090\n");
}

}  // namespace

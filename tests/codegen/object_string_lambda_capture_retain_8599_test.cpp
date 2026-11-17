// Codegen — inside a LAMBDA BODY, an `Object` local bound to a CAPTURED
// `string` took no reference of its own: the caller's release of the captured
// string freed it under any field the alias was stored into.
// §28.9 (Runtime-managed types), §8.6 (a reference type is not boxed),
// v0.357.
//
// THE DEFECT (issue #8599). The `Object := string` binding path that retains
// is the LocalDecl one: #2900 / #8307 / #8514 make `var o: Object := s;` a
// string-aliased slot that RETAINS at bind (and releases at scope end when the
// alias never escapes), so the source `s` keeps its own reference and its own
// scope-end release. That path runs at FUNCTION entry only — the
// `collectObjectStringReleaseEligible` collector is called on a function's
// statements and never on a lambda's. So inside a lambda body
// `var o: Object := x;` — `x` a CAPTURED `string` — was not a string-aliased
// slot at all: the bind-time retain did not fire (it is gated on
// `currentFn_ == objectStringReleaseEligibleFn_`, and the pin still named the
// ENCLOSING function), the slot took no reference of its own, and the
// caller's release of the captured string freed it under the alias.
//
// MEASURED on the pre-fix build, `--sanitize=address`, 200 iterations,
// `ASAN_OPTIONS=detect_leaks=1`: heap-use-after-free for both fix rows below
// (the issue's captured-parameter repro and the captured-local variant).
//
// THE FIX (§28.9, v0.357). §28.9 leaves a string's lifetime to the runtime,
// so a slot that can outlive the source must hold a reference: the lambda's
// own body runs the object-string eligibility collector (the way a function
// body does), the collector's token is pinned to the LAMBDA's function (so the
// bind-time retain fires inside the lambda), and the enclosing body's sets are
// put back afterwards (a nested collection must not disarm the enclosing
// body's own aliases). The reference is dropped again where the slot's own
// scope-end release is armed (the escape walk proved the alias never
// escapes); an alias that ESCAPES into a field keeps its reference — the
// documented residual LEAK of a string behind an `Object` erasure (#5059,
// #8459), never a dangling read.
//
// The heap-store rows run on `runAotCheckedLenient` (ASan armed, LSan off):
// the field's documented +1 leak is not this row's subject. The
// leak-checked row runs on `runAotChecked`. Every row asserts
// `!checkedToolchainSkipped(r)` before any guard that could absorb the
// skip.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The defect is a REFCOUNT defect: one iteration can hide it, 200 cannot.
constexpr int kIterations = 200;

// Sum over i in 0..199 of ("s" + i).Length: 10 * 2 + 90 * 3 + 100 * 4.
const char* kLengthTotal = "690\n";

// The declarations every row shares.
//   * `KeepLam` is the issue's own shape: a lambda body binds a CAPTURED
//     `string` PARAMETER into an `Object` local and stores the alias into the
//     field.
//   * `KeepLamLocal` is the captured-LOCAL variant: the lambda body binds a
//     local of the enclosing method, which the enclosing method's own scope
//     end releases.
//   * `ReadLam` is the read-only twin of `KeepLam`: the alias never leaves the
//     lambda, so the slot's own scope-end release is armed and the lambda's
//     retain balances it.
const char* kDecls =
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public method KeepLam(x: string) "
    "    var f := lambda(): int => do var o: Object := x; this.O := o; "
    "return 0; end; "
    "    var n := f(); "
    "  end KeepLam; "
    "  public method KeepLamLocal(x: string) "
    "    var s := x; "
    "    var f := lambda(): int => do var o: Object := s; this.O := o; "
    "return 0; end; "
    "    var n := f(); "
    "  end KeepLamLocal; "
    "  public method ReadLam(x: string): int "
    "    var f := lambda(): int => do var o: Object := x; "
    "return o.ToString().Length; end; "
    "    return f(); "
    "  end ReadLam; "
    "end Holder; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// `body` runs once per iteration with `h` (a fresh Holder) and `i` in scope;
// the program prints `total`.
std::string program(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; var total := 0; "
           "    while i < " + std::to_string(kIterations) + " do "
           "      var h := new Holder(); "
           + body +
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The issue's repro: the captured string is released by the enclosing `do`
// block before the field is read back.
std::string keepParamProgram() {
    return program("      do var x := Make.Str(i); h.KeepLam(x); end; "
                   "      var junk := Make.Str(99999); "
                   "      total := total + h.O.ToString().Length + "
                   "junk.Length - junk.Length; ");
}

// The captured-LOCAL variant: the lambda binds a local of the enclosing
// method, and the enclosing method's own scope end releases that local.
std::string keepLocalProgram() {
    return program("      do var x := Make.Str(i); h.KeepLamLocal(x); end; "
                   "      var junk := Make.Str(99999); "
                   "      total := total + h.O.ToString().Length + "
                   "junk.Length - junk.Length; ");
}

// The read-only twin: the alias is read inside the lambda and never stored.
std::string readProgram() {
    return program("      do var x := Make.Str(i); "
                   "total := total + h.ReadLam(x); end; ");
}

// ASan armed, LSan off: ran, exit 0, exact stdout. A use-after-free, double
// free or over-release fails the row; the field's documented +1 leak does not.
void expectNoUseAfterFree(const std::string& src, const std::string& expected) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ASan AND LSan armed: ran, exit 0, exact stdout, nothing leaked.
void expectCleanRun(const std::string& src, const std::string& expected) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN SHAPE AND ITS VARIANT (#8599).
// ===========================================================================

// The issue's repro: inside a lambda body, an `Object` local bound to a
// CAPTURED `string` and stored into a field. MEASURED heap-use-after-free on
// the pre-fix build.
BEZEL_TEST(object_string_lambda_capture_retain_8599,
           issue_lambda_body_captured_param_object_alias_stored_spec_28_9) {
    expectNoUseAfterFree(keepParamProgram(), kLengthTotal);
}

// The captured-LOCAL variant: the lambda body binds a local of the enclosing
// method (which that method's scope end releases) instead of a parameter.
// MEASURED heap-use-after-free on the pre-fix build.
BEZEL_TEST(object_string_lambda_capture_retain_8599,
           issue_lambda_body_captured_local_object_alias_stored_spec_28_9) {
    expectNoUseAfterFree(keepLocalProgram(), kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked.
// ===========================================================================

// BOUNDARY CONTROL (base-green, kept: an rc leak check a corpus case cannot
// express). The alias is only READ inside the lambda, so the slot's own
// scope-end release is armed and the lambda's retain balances it. Goes RED
// under the wrong fix `retain the captured string inside the lambda body but
// never arm the slot's own scope-end release` (one string per iteration
// leaks).
BEZEL_TEST(object_string_lambda_capture_retain_8599,
           control_lambda_body_captured_param_object_alias_read_only_spec_28_9) {
    expectCleanRun(readProgram(), kLengthTotal);
}

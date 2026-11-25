// Ownership — issue #9119: a lambda WRITE through a CAPTURED generic
// `ref p: T` parameter, at `T = string?` (the NULLABLE twin of #9077).
//
// Spec: §14.1 (v0.359) *Instantiation and its site* — "An instantiation of a
// generic declaration is the constructed type or constructed member obtained by
// substituting type arguments for its type parameters" (so in `M<string?>` the
// parameter `p: T` is a `string?` parameter); §22.3 (v0.359) *`ref` and `out`
// argument locations* — "A `ref` argument naming a mutable local or writable
// field is a true alias: callee writes are visible through the caller's
// location immediately"; §27.5 (v0.359) *Closures* — a captured `ref`
// parameter's modification is visible outside the lambda; §28.9 (v0.359)
// *Runtime-managed types* — `string` is reference-counted, every co-owner takes
// exactly one reference and releases exactly one.
//
// THE DEFECT. #9077's fix (19a3a830c) classified the captured alias at the
// CONCRETE substitution, but admitted only a NON-NULLABLE `string`: its
// predicate required `!r->nullable`, so the instantiation `M<string?>` — where
// the substitution binds the bare `T` to a NULLABLE string — was left out, and
// the write inside the lambda stayed a plain store. The caller's temporary is
// released at the end of the call's full expression, and the caller's later
// read of its own `ref` local reads freed memory.
//
// MEASURED on r30-c1 @ fec738642 (`bezelc --sanitize=address`, 200 calls):
// heap-use-after-free in `bz$P.Program$Run`, at the caller's read of its own
// `ref` local — identical on the base and on #9077's tip.
//
// THE FIX. The same capture is classified for the nullable substitution, so the
// write takes a reference of its own on the value it stores. The retain is
// NULL-SAFE: `bz_string_retain` returns early on null, so a `null` written
// through the alias is stored without a reference-count change and without a
// crash.
//
// ⚠ THE DISPLACED VALUE IS NOT RELEASED — the same recorded policy #9077's fix
// follows and #4965 records for the frame-level `ref`-typed-by-a-type-parameter
// store: the alias names the CALLER's location, whose value the callee does not
// own, so the callee may not drop a reference it never had. The displaced value
// leaks (issue #4980's policy). Never trade a leak for a crash.
//
// ⚠ Every argument is built at runtime (an interpolation): a constant-folded
// literal is an RC-SENTINEL on which retain/release are no-ops.
//
// Oracles. `runAotCheckedLenient` (AddressSanitizer on, LeakSanitizer off) —
// the displaced value leaks by the recorded policy above, so what these rows
// pin is the ABSENCE of the use-after-free, exactly as #4982's rows do. The
// plain AOT leg asserts the program's stdout and exit code.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
// The pre-fix use-after-free abort is exit 1.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// Run under AddressSanitizer + LeakSanitizer and return the number of leaked
// ALLOCATIONS LeakSanitizer counted (0 when it reported none). The run must
// have printed `expectedOut`, and NO memory error other than a leak may have
// fired: no AddressSanitizer report, no §28.9 over-release abort.
unsigned long long leakedAllocationsOf(const std::string& prog,
                                       const std::string& expectedOut) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, expectedOut);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("did not compile") == std::string::npos);
    unsigned long long bytes = 0, allocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, bytes, allocs);
    // A leak report whose SUMMARY could not be read is not "no leak".
    if (!sawSummary &&
        text.find("ERROR: LeakSanitizer") != std::string::npos)
        return ~0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << allocs << " allocation(s) / "
                  << bytes << " byte(s)\n";
    return sawSummary ? allocs : 0ull;
}

// =====================================================================
// (B1) The issue's verbatim repro: one env writes the alias; the CALLER
// reads its own `ref` local after the call. 200 calls.
// `q{j}`: |q-0|..|q-9| = 2, |q-10|..|q-99| = 3, |q-100|..|q-199| = 4
// ⇒ 10*2 + 90*3 + 100*4 = 690, plus M's 1 per call ⇒ 890.
// =====================================================================
const char* kIssueRepro = R"BZ(
module P;
public class Program
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s: string? := $"s{j}";
      t := t + Program.M<string?>(ref s, $"q{j}");
      t := t + (s ?? "").Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

// =====================================================================
// (B2) A NULL value written through the alias, then a non-null one. The
// first write stores null (the retain of a null `string?` is a no-op —
// `bz_string_retain` returns early on null), the second stores a runtime
// string; the CALLER's read of its own `ref` local must see the second
// write's value, alive.
// `rr{j}`: |rr-0|..|rr-9| = 3, |rr-10|..|rr-99| = 4, |rr-100|..|rr-199| = 5
// ⇒ 10*3 + 90*4 + 100*5 = 890, plus M's 1 per call ⇒ 1090.
// =====================================================================
const char* kNullWriteThenValue = R"BZ(
module P;
public class Program
  public static method M<T>(ref p: T, q: T, r: T): int
    var w := lambda(): int => do p := q; p := r; return 1; end;
    var a := w();
    return a;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s: string? := $"s{j}";
      t := t + Program.M<string?>(ref s, null, $"rr{j}");
      t := t + (s ?? "").Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
)BZ";

}  // namespace

// (B1) Pre-fix: heap-use-after-free in the caller's read of its own `ref`
// local after the call.
BEZEL_TEST(ownership_captured_ref_type_param_alias_nullable_9119,
           caller_reads_its_own_ref_local_after_the_write_spec_22_3) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro), "890\n");
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kIssueRepro, "890\n") <= 800ull);
    // The plain AOT leg: the same program's output and exit code.
    auto plain = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "890\n");
}

// (B2) Pre-fix: the same heap-use-after-free, with a NULL written through the
// alias first.
BEZEL_TEST(ownership_captured_ref_type_param_alias_nullable_9119,
           a_null_write_through_the_alias_then_a_value_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kNullWriteThenValue), "1090\n");
    BEZEL_EXPECT_TRUE(
        leakedAllocationsOf(kNullWriteThenValue, "1090\n") <= 800ull);
}

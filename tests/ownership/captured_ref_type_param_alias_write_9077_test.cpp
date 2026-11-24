// Ownership — issue #9077: a lambda WRITE through a CAPTURED generic
// `ref p: T` parameter, at `T = string`.
//
// Spec: §14.1 (v0.359) *Instantiation and its site* — "An instantiation of a
// generic declaration is the constructed type or constructed member obtained by
// substituting type arguments for its type parameters" (so in `M<string>` the
// parameter `p: T` is a `string` parameter); §22.3 (v0.359) *`ref` and `out`
// argument locations* — "A `ref` argument naming a mutable local or writable
// field is a true alias: callee writes are visible through the caller's
// location immediately"; §27.5 (v0.359) *Closures* — a captured `ref`
// parameter's modification is visible outside the lambda; §28.9 (v0.359)
// *Runtime-managed types* — `string` is reference-counted, every co-owner
// takes exactly one reference and releases exactly one.
//
// THE DEFECT. The `ref` twin of #9060 (the by-value `p: T` cell write). Every
// refcount decision about the alias is made on the SUBSTITUTED type — the
// alias's element type is the concrete `string` pointer — but the WRITE inside
// the lambda body was a plain store: lowerStringLocalReassign asks the
// UNSUBSTITUTED expression type, which is `T`. So the write neither retained
// the value it stored nor released the one it displaced, and the CALLER — whose
// `ref` local the alias names — read a freed string afterwards.
// MEASURED on r30-c1 @ 996375456 (`bezelc --sanitize=address`, 200 calls):
//
//   (A1) one env writes: the caller's read of its own `ref` local after the
//        call is a heap-use-after-free (the caller's temporary is released at
//        the end of the call's full expression);
//   (A2) two envs (a reader and a writer): the same heap-use-after-free.
//
// The non-generic twin (`M(ref p: string, q: string)`, same body) is clean:
// its write goes through the same helper and the target's expression type IS
// `string`. The generic method with only a READER lambda is clean too.
//
// THE FIX. The lambda's binding of such an alias (typeParamRefStringAliasHolders_,
// armed in lowerLambda) is admitted by lowerStringLocalReassign, so the write
// takes a reference of its own on the value it stores — the caller can read
// its own `ref` local afterwards.
//
// ⚠ THE DISPLACED VALUE IS NOT RELEASED. The alias names the CALLER's
// location, whose value the callee does not own: it may be a `T` local at
// `string`, which takes no +1 of its own (the #4980 `T`-local retain policy),
// so releasing the displaced value here would drop a reference the callee never
// had. The concrete `ref p: string` twin can release it, because a `string`
// location always owns its value; the generic alias cannot know, so it
// retains-new only and the displaced value leaks. That is the SAME trade
// #4965's `ref`-typed-by-a-type-parameter store makes in the frame (and the
// one #4982's regression test records: "the `ref U` store at `U = string` is
// retain-only by the recorded policy of #4965 — the overwritten old value
// leaks, which is issue #4980"). MEASURED: releasing the displaced value
// instead turns the generic-caller shape below into a heap-use-after-free.
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
// (A1) The issue's verbatim repro: one env writes the alias; the CALLER
// reads its own `ref` local after the call. 200 calls.
// `s{j}` / `q{j}`: |q-0|..|q-9| = 2, |q-10|..|q-99| = 3, |q-100|..|q-199| = 4
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
      var s := $"s{j}";
      t := t + Program.M<string>(ref s, $"q{j}");
      t := t + s.Length;
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
// (A2) Two envs: a reader and a writer over the same alias. The reader
// runs after the write, so it reads the value the write stored.
// M returns 1 (the writer) + 1 (the reader) per call ⇒ 200*2 + 690 = 1090.
// =====================================================================
const char* kTwoEnvs = R"BZ(
module P;
public class Program
  public static method Use<T>(x: T): int
    var y := x;
    return 1;
  end Use;
  public static method M<T>(ref p: T, q: T): int
    var r := lambda(): int => Program.Use<T>(p);
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := $"s{j}";
      t := t + Program.M<string>(ref s, $"q{j}");
      t := t + s.Length;
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
// (A3) BOUNDARY CONTROL: the alias names a `T` local of a GENERIC caller,
// which takes no +1 of its own (the #4980 `T`-local retain policy). The
// callee therefore may NOT release the displaced value — doing so drops a
// reference it never had, and the caller's own local dangles.
// This row is GREEN on the base (the pre-fix write is a plain store, which
// displaces nothing) and RED under the "release the displaced value" fix.
// =====================================================================
const char* kGenericCallerTypeParamLocal = R"BZ(
module P;
public class Program
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a;
  end M;
  public static method Caller<T>(x: T, q: T): int
    var s: T := x;
    var a := Program.M<T>(ref s, q);
    return a;
  end Caller;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s2 := $"s{j}";
      t := t + Program.Caller<string>(s2, $"q{j}");
      t := t + s2.Length;
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

// (A1) Pre-fix: heap-use-after-free in the caller's read of `s`.
BEZEL_TEST(ownership_captured_ref_type_param_alias_9077,
           caller_reads_its_own_ref_local_after_the_write_spec_22_3) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro), "890\n");
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kIssueRepro, "890\n") <= 800ull);
    // The plain AOT leg: the same program's output and exit code.
    auto plain = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, "890\n");
}

// (A2) Pre-fix: heap-use-after-free.
BEZEL_TEST(ownership_captured_ref_type_param_alias_9077,
           second_env_reads_the_written_alias_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kTwoEnvs), "1090\n");
    BEZEL_EXPECT_TRUE(leakedAllocationsOf(kTwoEnvs, "1090\n") <= 800ull);
}

// (A3) The boundary control: the alias names a `T` local that owns no +1, so
// the displaced value must NOT be released.
BEZEL_TEST(ownership_captured_ref_type_param_alias_9077,
           generic_caller_alias_type_param_local_keeps_its_value_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kGenericCallerTypeParamLocal), "890\n");
    BEZEL_EXPECT_TRUE(
        leakedAllocationsOf(kGenericCallerTypeParamLocal, "890\n") <= 800ull);
}

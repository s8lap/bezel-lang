// Regression test for issue #9230 — an indexer whose INDEX PARAMETER is
// FUNCTION-TYPED admitted a lambda whose inferred return type does not match
// the slot, and the accessor then called the closure through the SLOT's ABI:
// the program compiled, linked and SEGFAULTed at run time.
//
// THE DEFECT. §27.3 (v0.359) *Admission at a target-typed non-call position*,
// verbatim: "the body is typed, and its inferred return type must equal the
// target's return slot or implicitly converts to it (§5.1); otherwise the
// lambda is a compile error at the position". §23.4 (v0.359) makes an
// indexer's parameter list a parameter list ("Indexers may be overloaded by
// parameter type list"), so the index argument's slot is the function-typed
// TARGET that rule judges. §22.6 (v0.359) *Lambda-literal arguments* grades
// the position by the lambda's INFERRED RETURN TYPE: "the candidate is
// applicable at the position when the lambda's inferred return type equals
// the candidate's return slot, implicitly converts to it (§5.1), or binds it".
//
// THE METHOD TWIN IS THE ORACLE, AND IT WAS ALREADY CORRECT. The matched
// formal's function-type return slot is threaded into the lambda's own visit
// (src/sema/ExprTypecheck_Exprs.cpp's `pendingLambdaTargetReturn_`), and the
// lambda arm then applies the check with
// `checkAssignable(<the body's value type>, <the slot's return slot>,
// <the body's span>, "lambda body's returned value", <the body>)` — so
// `c.M(lambda() => 7)` at `M(f: () -> string)` is rejected with
//
//   "the lambda body's returned value has a type that is not implicitly
//    convertible to the target type (\xC2\xA7""5.1)"
//
// THE INDEX PATH NEVER SUPPLIED THAT TARGET. The index argument is visited
// with NO target (the IndexExpr's overload selection runs LATER — after the
// argument has been visited), so the §27.3 check never ran; and
// `checkAssignable`'s function-to-function arm deliberately skips the SHAPE
// check for a fresh lambda ("a lambda literal's param/return shape is
// target-typed — §27.4 — so there is no prior, independently-typed value to
// compare; its body is separately checked against the target elsewhere"), and
// that "elsewhere" is the §27.3 check the index path never reaches.
//
// MEASURED on r30-c1 @ fc77e935f (the base), `scripts/agent_check.sh --probe
// --asan` on the issue's own repro:
//
//   ==2268033==ERROR: AddressSanitizer: SEGV on unknown address
//   0x00000000000f (pc 0x0000003787b9 ...)
//       #0 ... in bz$indexer$get
//       #1 ... in main
//
// — byte-identical to the issue's report. The closure returns an `i32` (the
// lambda's inferred return type, §27.3); the accessor calls it as a
// `ptr`-returning function (the slot's declared type) and reads `.Length` of
// the garbage.
//
// THE FIX. `ExprTC::checkIndexerLambdaArgAdmission` (src/sema/ExprTypecheck_
// Exprs.cpp) — the indexer twin of the METHOD path's own check, on the same
// values and with the SAME diagnostic. It is SILENT when the receiver
// declares an OVERLOAD SET of arity-matching indexers: the resolution's pick
// then governs, and §22.6's exact-match ranking grades it (#9187) — the
// indexer twin of #6161's `isSoleNamedCandidate` guard, which the METHOD
// path's own check carries.
//
// ROWS (all RED on r30-c1 @ fc77e935f, all GREEN with the fix):
//   Row 1 — the GET accessor, the issue's own repro: `c[lambda() => 7]` at
//           `this[f: () -> string]`. RED on the base (the SIGSEGV), GREEN
//           with the fix (a compile-time rejection with the §5.1 diagnostic).
//   Row 2 — the SET accessor twin: `c[lambda() => 7] := 3;`. RED on the base
//           (ADMITTED — the setter does not call the closure, so the base
//           ran it), GREEN with the fix (the same rejection).
//   Row 3 — the COMPOUND-ASSIGNMENT twin: `c[lambda() => 7] += 3;`. RED on
//           the base (ADMITTED), GREEN with the fix (the same rejection).
//   Row 4 — the INTERFACE-typed receiver: the same read through an
//           interface-typed variable, so the accessor dispatches through the
//           itable. RED on the base (the SIGSEGV), GREEN with the fix.
//   Row 5 — the GREEN CONTROL the brief names: a MATCHING-return lambda at
//           the same slot (`c[lambda() => "hello"]`) is still ADMITTED and
//           still RUNS, 200 iterations, with a `runAotChecked` leak leg.
//           This is the suite's boundary control: it is the check a corpus
//           case cannot express (the leak oracle over 200 iterations, §28.9 /
//           §28.14 — a closure is runtime-managed and its reference must be
//           dropped exactly once), and it is RED under a plausible wrong fix
//           — one that over-rejects and refuses the matching lambda too.
//           Green on the base, so it pins nothing about the DEFECT; it pins
//           that the fix did not over-reject.
//
// The green-on-base twins the issue names — the METHOD twin, and the indexer
// OVERLOAD SET twin (#9187's shape) — are NOT gtest rows: they are left with
// the corpus lane under
// /var/tmp/bezel-scratch/r43-m113-9230/probes/corpus-candidates/ with their
// deciding spec sentences.
//
// per §5.1, §22.6, §23.4, §27.3, §27.4, §28.9, §28.14 (issue #9230)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The EXACT diagnostic the METHOD twin draws — the indexer path must draw the
// SAME one (the issue's words: "the indexer argument must get the SAME
// target-typed check and the SAME diagnostic").
const char* kLambdaBodyDiagnostic =
    "the lambda body's returned value has a type that is not implicitly "
    "convertible to the target type (\xC2\xA7""5.1)";

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

// The MEMORY oracle: the checked run leaked NOTHING at all, and no memory
// error fired.
void expectNoLeak(const JitResult& r) {
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

// The REJECTION oracle: the driver rejected the program before ever running
// it, with a REAL source location, and the diagnostic is the §5.1 one the
// METHOD twin draws.
//
// `failedAtCompileTimeWithRealLocation` alone would NOT establish this: the
// AOT harness's `ran` is false for a crash, a signal, a timeout and a forced
// ASan report just as it is for a genuine rejection (#3892) — and the base
// CRASHES. So the row asserts the SPECIFIC diagnostic by name as well, which
// is what makes it red on the base (a crash) and green with the fix (a
// located §5.1 rejection).
void expectRejectedWithLambdaBodyDiagnostic(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kLambdaBodyDiagnostic));
}

// ── Row 1 (§27.3 / §22.6 / §23.4 / §5.1): the GET accessor — the issue's
// own repro, verbatim.
//
// The accessor CALLS its parameter (`f().Length`), so on the base the closure
// is called through the slot's `ptr`-returning ABI while the closure returns
// an `i32` — the SIGSEGV the issue reports.
const char* kGetProgram = R"(
module Fn9230G;
public class FlFn9230G
  public property this[f: () -> string]: int
    get => do return f().Length; end;
  end;
  public constructor() end constructor;
end FlFn9230G;
public class ProgramFn9230G
  public static method Main(): int
    using var c := new FlFn9230G();
    Console.WriteLine(c[lambda() => 7]);
    return 0;
  end Main;
end ProgramFn9230G;
)";

// ── Row 2 (§27.3 / §23.3 / §23.4): the SET accessor twin.
//
// The setter does not call the closure, so the base ran it (no crash) — but
// it ADMITTED the same mismatched lambda, which is the same defect: the
// index argument's slot is the target, whatever the accessor does with it.
const char* kSetProgram = R"(
module Fn9230S;
public class FlFn9230S
  public property this[f: () -> string]: int
    get => do return f().Length; end;
    set => do end;
  end;
  public constructor() end constructor;
end FlFn9230S;
public class ProgramFn9230S
  public static method Main(): int
    using var c := new FlFn9230S();
    c[lambda() => 7] := 3;
    return 0;
  end Main;
end ProgramFn9230S;
)";

// ── Row 3 (§27.3 / §23.4): the COMPOUND-ASSIGNMENT twin.
//
// `c[lambda() => 7] += 3;` reads the indexer and writes it back, so the
// index argument is the SAME slot — and the base ADMITTED it (with an
// unrelated #4045 internal note about the store).
const char* kCompoundProgram = R"(
module Fn9230C;
public class FlFn9230C
  public property this[f: () -> string]: int
    get => do return f().Length; end;
    set => do end;
  end;
  public constructor() end constructor;
end FlFn9230C;
public class ProgramFn9230C
  public static method Main(): int
    using var c := new FlFn9230C();
    c[lambda() => 7] += 3;
    return 0;
  end Main;
end ProgramFn9230C;
)";

// ── Row 4 (§27.3 / §11.2 / §22.6 / §23.4): the INTERFACE-typed receiver.
//
// The same read through an interface-typed variable, so the accessor
// dispatches through the itable — the slot's ABI all the same.
const char* kInterfaceProgram = R"(
module Fn9230I;
public interface IFlFn9230I
  readonly property [f: () -> string]: int;
end IFlFn9230I;
public class FlFn9230I : IFlFn9230I
  public property this[f: () -> string]: int
    get => do return f().Length; end;
  end;
  public constructor() end constructor;
end FlFn9230I;
public class ProgramFn9230I
  public static method Main(): int
    using var c := new FlFn9230I();
    var fl: IFlFn9230I := c;
    Console.WriteLine(fl[lambda() => 7]);
    return 0;
  end Main;
end ProgramFn9230I;
)";

// ── Row 5 (§27.3 / §5.1 / §28.9 / §28.14): the GREEN CONTROL — a
// MATCHING-return lambda at the same slot.
//
// The lambda's inferred return type is `string`, the slot's return slot is
// `string`, so §27.3 admits it and the accessor calls it through its own ABI.
//
// 200 iterations: 200 * 5 ("hello".Length) = 1000. The 200 iterations are the
// leak oracle's window (§28.9 / §28.14: the closure object is runtime-
// managed and its reference must be dropped exactly once, so a leaked closure
// is visible only over many iterations).
const char* kMatchingProgram = R"(
module Fn9230M;
public class FlFn9230M
  public property this[f: () -> string]: int
    get => do return f().Length; end;
  end;
  public constructor() end constructor;
end FlFn9230M;
public class ProgramFn9230M
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new FlFn9230M();
      t := t + c[lambda() => "hello"];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramFn9230M;
)";

BEZEL_TEST(indexer_fn_param_lambda_return_admission_9230,
           get_accessor_rejects_a_lambda_whose_return_does_not_match_the_slot) {
    expectRejectedWithLambdaBodyDiagnostic(runAot(kGetProgram));
}

BEZEL_TEST(indexer_fn_param_lambda_return_admission_9230,
           set_accessor_rejects_a_lambda_whose_return_does_not_match_the_slot) {
    expectRejectedWithLambdaBodyDiagnostic(runAot(kSetProgram));
}

BEZEL_TEST(indexer_fn_param_lambda_return_admission_9230,
           compound_assignment_rejects_a_lambda_whose_return_does_not_match_the_slot) {
    expectRejectedWithLambdaBodyDiagnostic(runAot(kCompoundProgram));
}

BEZEL_TEST(indexer_fn_param_lambda_return_admission_9230,
           interface_typed_receiver_rejects_a_lambda_whose_return_does_not_match_the_slot) {
    expectRejectedWithLambdaBodyDiagnostic(runAot(kInterfaceProgram));
}

BEZEL_TEST(indexer_fn_param_lambda_return_admission_9230,
           matching_return_lambda_is_still_admitted_and_runs_control_spec_27_3) {
    auto v = runAot(kMatchingProgram);
    BEZEL_EXPECT_TRUE(v.ran);
    BEZEL_EXPECT_EQ(v.exitCode, 0);
    BEZEL_EXPECT_EQ(v.stdoutText, "1000\n");
    auto r = runAotChecked(kMatchingProgram);
    expectNoLeak(r);
}

}  // namespace

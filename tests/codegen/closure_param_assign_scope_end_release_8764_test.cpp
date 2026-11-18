// Regression tests for #8764: assigning a lambda to a function-typed
// PARAMETER inside a body leaks the REPLACEMENT closure (the method form and
// the constructor form leak identically).
//
// THE RULE (spec v0.359).
//
//   §28.9 "Runtime-managed types": "the type's lifetime is managed by the
//   runtime without user involvement" — and, for function-typed values,
//   "Runtime management governs the *function value itself* — the
//   heap-allocated closure object that backs it". §28.9 also says a
//   function-typed value "carries no disposal obligation": the reclamation is
//   the RUNTIME's, not the program's — which is exactly why a closure the
//   frame holds a reference to and drops at scope end must be reclaimed
//   there. §27.5 ("Closures") is the section that makes the closure a
//   heap-allocated object with its own lifetime.
//
//   §28.14 "Scoped callback parameters" is the section that says what the
//   frame MAY do with a plain function-typed parameter: it is call-bounded —
//   "the callee receives the callback for the duration of the call only" and
//   "Within the callee the callback may be invoked, bound to locals, and
//   passed on as an argument to another call-bounded parameter" — so the
//   callee may NOT retain the CALLER'S callback past the call. Assigning a
//   NEW closure to the parameter's slot is not a retention of the caller's
//   callback (the caller's value is dropped, not kept), and the frame's own
//   reference to the value it drops must be given back when the frame ends.
//
// THE DEFECT (MEASURED on r30-c1 @ c4fecbb3c, `bezelc --sanitize=address`):
// the issue's own repro leaks "3600 byte(s) leaked in 100 allocation(s)" over
// 50 iterations; the constructor spelling leaks the same. The frame's +1 on
// the REPLACEMENT closure — the one the body installed in the parameter's
// slot — was never given back.
//
// ROOT CAUSE. `Emitter::retainAssignedClosureParams` (fix #4174 follow-up)
// gives an assigned-to closure-typed parameter an independent +1 at bind, and
// `Emitter::lowerClosureLocalReassign` (fix #4174) keeps the slot at exactly
// one reference across every overwrite (retain-or-transfer the new value,
// release the displaced old one). So the frame owns exactly ONE reference to
// whatever the slot holds at scope end — the caller's argument (if no
// overwrite ran) or the replacement closure (if one did) — and NOTHING
// released it. The `string` twin of the same machinery already had the
// missing half: `Emitter::retainAssignedStringParams` (fix #5253) queues a
// scope-end release of the parameter's FINAL value through
// `stringParamReleaseEligible`. This file's fix gives the closure side the
// same shape (`Emitter::closureParamReleaseEligible`).
//
// The old `tests/codegen/closure_local_reassign_release_4174_test.cpp`
// documented the missing half as a deliberate trade-off ("a reassigned
// local's (or an assigned-to parameter's) final value leaks safely"); that
// trade-off is what #8764 closes.
//
// The rows below loop (50-200 iterations) so the leak cannot hide under the
// sanitizer's noise, and each runs under `runAotChecked` (generated-code
// AddressSanitizer AND LeakSanitizer), where `r.ran` is itself the leak
// assertion.
//
// per §27.5, §28.9, §28.14 (v0.359)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
// The program's stdout when it compiled, ran to exit code 0 and no sanitizer
// (ASan or LSan) fired; a marker otherwise, so a leak (which the checked
// harness reports as a sanitizer firing) can never compare equal to an
// expectation.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
// Generated-code ASan ON, LeakSanitizer OFF: for the row whose leak is
// UNRELATED to this fix and pre-exists on both arms (the caller's own
// function-typed local escapes as a call argument, so the local's escape walk
// declines its scope-end release — the pre-existing "an escaping borrow local
// keeps its +1" trade-off). A use-after-free, a double-free or the runtime's
// over-release abort still fails the row.
std::string runLenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
}  // namespace

// THE ISSUE'S FIRST REPRO, VERBATIM (50 iterations): a method assigns a lambda
// to its function-typed parameter and calls it. Pre-fix: 3600 bytes leaked in
// 100 allocations over the 50 iterations; with the fix: clean, "450".
BEZEL_TEST(closure_param_assign_scope_end_release_8764,
           issue_repro_method_param_assign_leak_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module T;
public class H
  public v: int;
  public method Run(f: () -> int)
    f := lambda() => 9;
    v := f();
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var tot := 0;
    using var h := new H();
    while i < 50 do h.Run(lambda() => 2); tot := tot + h.v; i := i + 1; end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
                     "450");
}

// THE ISSUE'S SECOND REPRO (the constructor spelling): the same leak, the
// constructor's own body. Pre-fix: the same 3600 bytes in 100 allocations;
// with the fix: clean, "450".
BEZEL_TEST(closure_param_assign_scope_end_release_8764,
           issue_repro_constructor_param_assign_leak_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module T;
public class H
  public v: int;
  public constructor(f: () -> int)
    f := lambda() => 9;
    v := f();
  end constructor;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var tot := 0;
    while i < 50 do
      using var h := new H(lambda() => 2);
      tot := tot + h.v;
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
                     "450");
}

// THE ACCOUNTING, TWICE OVER: the frame owns exactly ONE reference to the
// slot's value at any time, so a SECOND overwrite releases the first
// replacement and the scope end releases the second. Pre-fix: both
// replacements leaked (128 bytes in 4 allocations per iteration); with the
// fix: clean, "900" over 100 iterations.
BEZEL_TEST(closure_param_assign_scope_end_release_8764,
           two_replacements_param_assign_leak_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module T;
public class H
  public v: int;
  public method Run(f: () -> int)
    f := lambda() => 7;
    f := lambda() => 9;
    v := f();
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var tot := 0;
    using var h := new H();
    while i < 100 do h.Run(lambda() => 2); tot := tot + h.v; i := i + 1; end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
                     "900");
}

// THE CONDITIONAL OVERWRITE: on the iterations where the branch does NOT
// run, the slot still holds the CALLER'S callback, and the frame's own +1 on
// it (taken at bind by retainAssignedClosureParams) is what the scope end
// gives back — the caller's own reference is untouched (§28.14: the callback
// is the caller's for the duration of the call and after it). On the
// iterations where the branch DOES run, the scope end gives back the
// replacement's +1 instead. Pre-fix: the replacement leaked on the branch
// iterations AND the frame's +1 on the caller's callback leaked on the others;
// with the fix: clean. The expected sum is computed here, in C++: the branch
// runs on the first 50 of the 100 iterations, so 50 x 9 + 50 x 2.
BEZEL_TEST(closure_param_assign_scope_end_release_8764,
           conditional_replacement_param_assign_leak_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module T;
public class H
  public v: int;
  public method Run(f: () -> int, c: bool)
    if c then
      f := lambda() => 9;
    end if;
    v := f();
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var j := 0; var tot := 0;
    using var h := new H();
    while i < 100 do
      h.Run(lambda() => 2, j < 50);
      tot := tot + h.v;
      i := i + 1;
      j := j + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
                     "550");
}

// THE BOUNDARY CONTROL (green on the base AND with the fix; red under a
// plausible WRONG fix — see the commit body): the CALLER'S own binding must
// survive the callee's scope-end release. §28.14: a plain function-typed
// parameter is call-bounded — "the callee receives the callback for the
// duration of the call only" — so the callee's frame-end release gives back
// the frame's OWN reference and never the caller's.
//
// A wrong fix that releases the parameter's slot at scope end WITHOUT the
// bind-time retain (retainAssignedClosureParams) would free the caller's
// callback at the callee's first overwrite — a heap-use-after-free read by
// the caller below, which this row catches.
//
// The row runs with LeakSanitizer OFF (runAotCheckedLenient) because the
// caller's own function-typed local escapes as a call ARGUMENT here — the
// pre-existing "an escaping borrow local keeps its +1" trade-off, unrelated
// to this fix and present on both arms. A use-after-free, a double-free or
// the runtime's over-release abort still fails the row.
BEZEL_TEST(closure_param_assign_scope_end_release_8764,
           control_caller_argument_binding_intact_spec_28_14) {
    BEZEL_EXPECT_EQ(runLenientOut(R"BZL(
module T;
public class H
  public static method Run(f: () -> int): int
    f := lambda() => 99;
    return f();
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var h: () -> int := lambda() => 5;
    var r := H.Run(h);
    Console.Write("r=" + r.ToString() + " h=" + h().ToString());
    return 0;
  end;
end;
)BZL"),
                     "r=99 h=5");
}

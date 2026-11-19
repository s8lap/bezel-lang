// Codegen — issue #8903: a collection-literal-initialised `Array<int>` local
// that is DECLARED INSIDE A LAMBDA BODY and only read was never freed. The
// array header and its data block (160 B in 2 allocations) leaked once per
// call of the lambda.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a `Array<T>` is
// trivial-dispose — `Dispose()` is a no-op — but its backing storage is part
// of the instance's single allocation and is released with it (§28.11,
// "Borrowing collections"). K1.5 (§28.3.1, v0.359, "The ownership kernel",
// item 5
// "Atoms"): a reference-typed collection literal is a FRESH PRODUCTION and so
// an OWNED atom; the holder's scope end discharges it. §15.4 (v0.359,
// "Collection literals") is the spelling that produces it.
//
// ROOT CAUSE. The scope-end reclaim of a borrowing-`Array<T>` literal local is
// registered by `lowerLocalDecl` only when
// `collectBorrowingArrayFreeEligible`'s escape walk proved the local never
// escapes its scope, and the registration is gated on
// `currentFn_ == borrowingArrayFreeEligibleFn_` — the function the walk
// analysed. `emitFunctionBody` (a METHOD body) and `emitConstructorBody` run
// that pass for their own body; the SYNC LAMBDA body — emitted by
// `Emitter::lowerLambda` into its own `lam$N` function — never did. So the
// lambda's own `var ys: Array<int> := [1, 2];` was never in
// `borrowingArrayFreeEligible_` at all, and the fn token named the ENCLOSING
// function, so the registration site could not fire for it either. The
// byte-identical METHOD body was clean (issue #8903's own control), which is
// what isolates the venue as the defect.
//
// The fix runs the pass for the lambda's own body and keys it to the lambda's
// own function. It is the same pass the method and constructor venues run, so
// the verdict is the same verdict — a local the walk proves non-escaping is
// reclaimed at the lambda's scope end, and a local the walk proves ESCAPING
// is left alone (a leak, the safe direction). The two boundary controls below
// pin that second half.
//
// ORACLE. `runAotChecked` (generated-code AddressSanitizer + LeakSanitizer):
// a leak of this shape is reported as a failed run, so a RED row here is the
// leak and a GREEN row is its absence. The rows call the leaking shape 200
// times, so a leak is 400 allocations and cannot hide under the sanitizer's
// noise (one leak of this shape is reported only intermittently).
//
// The two BOUNDARY CONTROLS are base-green by construction — the sound
// codegen verdict for an array that ESCAPES is the conservative one, which
// keeps (leaks) it, and the sound verdict for the ENCLOSING body's own
// literal is the one the method venue has always given. Each is red under a
// plausible wrong fix named in the commit body: control 1 under "omit the
// save/restore of the borrowing-reclaim sets around the nested body
// emission", control 2 under "reclaim the array at the lambda's scope end
// without consulting the escape walk".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// stdout of a checked run (ASan + LSan) that compiled, ran and exited 0, or a
// marker naming what failed.
std::string checkedOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// stdout of a lenient run (ASan, no LSan) that compiled, ran and exited 0, or
// a marker naming what failed.
std::string lenientOut(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's literal repro: a collection-literal `Array<int>` local declared
// in a LAMBDA body and only read. 200 calls * 2 elements = 400.
//
// RED on the base (r30-c1): "AddressSanitizer: 32000 byte(s) leaked in 400
// allocation(s)" — the array header and its data block, once per call.
BEZEL_TEST(lambda_body_literal_local_leak_8903,
           issue_repro_literal_local_in_lambda_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
class C
  public method Go(cond: bool): int
    var f := lambda(): int => do
      var ys: Array<int> := [1, 2];
      return ys.Count;
    end;
    return f();
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..200 do
      using var c := new C();
      n := n + c.Go(false);
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
              "400\n");
}

// The issue's ALIAS twin: a borrow alias of the same local (`var b := ys;`)
// leaked the same amount, because the alias is a borrow of the same backing
// (K3.13 / §28.11) and does not make the local an escape. 200 * 2 = 400.
//
// RED on the base (r30-c1): the same 32000 B / 400 allocations.
BEZEL_TEST(lambda_body_literal_local_leak_8903,
           issue_repro_alias_of_literal_local_in_lambda_body_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
class C
  public method Go(cond: bool): int
    var f := lambda(): int => do
      var ys: Array<int> := [1, 2];
      var b := ys;
      return ys.Count + b.Count - 2;
    end;
    return f();
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..200 do
      using var c := new C();
      n := n + c.Go(false);
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
              "400\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL 1 of 2: the ENCLOSING body's own literal is still
// reclaimed after a lambda body has run.
//
// THE RULE. §28.9 (v0.359): the backing storage of a borrowing `Array<T>` is
// reclaimed with the instance at the OWNER's scope end. The owner here is the
// METHOD, not the lambda: the lambda's body is emitted in the MIDDLE of the
// method's body, so the method's own eligibility verdict must survive the
// nested emission unchanged.
//
// The control is a LEAK check (`runAotChecked`, ASan + LSan): the method's
// literal is declared AFTER the lambda, so its scope-end registration is made
// after the nested body has run. The lambda body declares NO collection
// literal of its own, so the only array in the program is the method's — the
// row is therefore green on the base arm (the method venue has always
// reclaimed it) and green with the fix. It is red under a plausible wrong
// fix: omitting the save/restore of the borrowing-reclaim sets around the
// nested body emission (the lambda's own verdict and fn token would still be
// in place, so the method's registration would never fire and the method's
// literal would leak — MEASURED 32000 B / 400 allocations over 200 calls).
//
// 200 calls * (7 + 2 - 2) = 1400.
// ---------------------------------------------------------------------------

BEZEL_TEST(lambda_body_literal_local_leak_8903,
           boundary_control_enclosing_body_literal_still_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
class C
  public method Go(): int
    var f := lambda(): int => do
      var k := 7;
      return k;
    end;
    var ys: Array<int> := [1, 2];
    return f() + ys.Count - 2;
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..200 do
      using var c := new C();
      n := n + c.Go();
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
              "1400\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL 2 of 2: a NESTED lambda's read carries the array's
// storage out of the lambda body.
//
// THE RULE. §28.9 (v0.359): runtime management of a closure "relaxes nothing
// about the values the closure captures: a captured borrowed class reference
// is subject to §28.3's borrow-escape rules". `ys.GetEnumerator()` is a BORROW
// of the array's backing storage, not a copy of it — so the array must NOT be
// reclaimed at the lambda body's scope end while the enumerator the nested
// lambda returned is still read by the caller.
//
// The control pins MEMORY SAFETY and the output under
// `runAotCheckedLenient` (ASan on, LSan off): the sound codegen verdict for an
// array that ESCAPES is the conservative one, which keeps (leaks) it. It is
// red under a plausible wrong fix: reclaiming the array at the lambda's scope
// end WITHOUT consulting the escape walk (the enumerator then reads reclaimed
// storage — heap-use-after-free).
//
// 100 calls * (1 + 2 + 3) = 600.
// ---------------------------------------------------------------------------

BEZEL_TEST(lambda_body_literal_local_leak_8903,
           boundary_control_nested_lambda_returns_enumerator_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method MakeEnum(): IEnumerator<int>
    var f := lambda(): IEnumerator<int> => do
      var ys: Array<int> := [1,2,3];
      var g := lambda() => ys.GetEnumerator();
      return g();
    end;
    return f();
  end MakeEnum;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do
      var e := MakeEnum(); while e.MoveNext() do s := s + e.Current; end;
    end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

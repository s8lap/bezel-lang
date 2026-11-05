// Regression tests for #8058 — a function value's closure CELL is never
// reclaimed at exit (64 bytes leaked per value, §28.9).
//
// THE DEFECT.
//
//   §28.9 (v0.355) puts the function value on the runtime-managed tier and
//   assigns its LIFETIME to the runtime, verbatim:
//
//     "Runtime management governs the *function value itself* — the
//      heap-allocated closure object that backs it — and relaxes nothing
//      about the values the closure captures"
//
//   and, in the same subsection:
//
//     "**Function-typed values** — values of any function type
//      `(T1, ..., Tn) -> R`, regardless of whether they capture. […]
//      Function-typed fields, parameters, and locals are not subject to
//      `owned` annotations and carry no disposal obligation"
//
//   So the CLOSURE CELL — the heap object behind the value — is the runtime's
//   to reclaim. Nothing in the USER's program has to do it.
//
//   MEASURED on the base tree (this lane's base `8845ad4e`, Spec v0.355),
//   through the route the issue names (`g.ToString()`), with
//   `scripts/agent_check.sh --probe --asan`:
//
//     ==================================================================
//     ==NNNN==ERROR: LeakSanitizer: detected memory leaks
//
//     Direct leak of 64 byte(s) in 1 object(s) allocated from:
//         #0 ... in calloc
//         #1 ... in main
//     SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).
//
//   THE MECHANISM (MEASURED, not assumed). The runtime ALREADY has the
//   release path — `bz_closure_release` (`src/runtime/bezel_runtime.h:1554`,
//   `src/runtime/bezel_runtime_refcounting.cpp:562`) is rc-based and frees the
//   cell + its env block at the last drop. What was missing was the CALL to
//   it: the codegen escape analysis (`ClosureEscapeWalker`, in
//   `src/codegen/LlvmBackend_Setup.cpp`) classified the RECEIVER of a member
//   access as an ESCAPE, so the local never entered `closureReleaseEligible_`
//   and its scope-end `bz_closure_release` never armed.
//
//   MEASURED on the base tree by the emitted object's symbol table
//   (`build/bezelc -c -o fv.o fv.bzl; nm -u fv.o`):
//
//     var g := lambda(...) => ...;                -> bz_closure_release IS
//                                                    referenced (armed)
//     var g := lambda(...) => ...; g.ToString();   -> bz_closure_release is
//                                                    NOT referenced (not armed)
//
//   ⇒ the 64-byte cell had NO releaser at all.
//
// WHY THE ESCAPE ANALYSIS WAS WRONG. The arm's stated premise was "Member
// access / indexing / interpolation do not apply to a function value" — and
// §8.1 (v0.355) makes that false: a function type reaches `Object` "via [its]
// reference-type heap representation", and §8.5 (v0.355) renders a function
// value through `Object.ToString()` — so the member-access shape is the
// DOCUMENTED rendering route (issue #7592's own regression row exercises it).
//
// WHEN THE RECEIVER MAY BE RELEASED AT SCOPE END — and when it may NOT.
// §8.1 (v0.355) declares `Object`'s instance surface as `GetType`, `Equals`,
// `GetHashCode` and `ToString`, with "method bodies ... provided by the
// runtime". When a call reaches one of THOSE bodies, no user code runs on
// the receiver, so the call cannot store it anywhere that outlives the call:
// the receiver's own reference stays in its slot, and the slot's scope-end
// release is exactly-once. §28.3.8 clause 4 (v0.355) draws
// the same line: "A function value as the receiver of a member `Object`
// declares (`f.ToString()`, `f.GetHashCode()`, an interpolation hole) is not
// at a set-erasing position; as the receiver of an extension call whose head
// is `Object` [...] it is". An `extension Object` member is USER code that
// "may pass [`this`] on to a parameter that stores it" (§9.7, v0.355), so
// its receiver keeps the conservative ESCAPE. The first version of this fix
// (83c9a9cd) relaxed EVERY member call and turned an extension's stored
// alias into a heap-use-after-free; the FIX-FORWARD rows at the end of this
// file pin that line. The second version (eb91cd7a) relaxed by member NAME,
// on §26.5 rule 1 ("Instance members win — by name"); but sema does not
// enforce rule 1 everywhere (#8051): an `extension Object` member that
// REUSES `ToString` / `Equals` / `GetHashCode` at another arity is silently
// bound, and the name-keyed relaxation freed its receiver under a live
// alias. The walker now asks what the call RESOLVED TO (the callee's
// `exprSymbol`: an extension `MethodSymbol`, or none for `Object`'s own
// runtime-provided member); the SAME-NAMED-EXTENSION rows at the end of this
// file pin that.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
// The defect is a LEAK, so the oracle is the STRICT harness (`runAotChecked`):
// it links the generated code against the AddressSanitizer runtime and runs it
// with `detect_leaks=1`, so a leaked allocation FAILS the row.
//
// THE LOOP IS LOAD-BEARING, and not out of caution. MEASURED: a SINGLE
// leaked allocation is only SOMETIMES reported — the same binary, run 20
// times, reported the leak in 8 runs and not in 12 (the closure cell's pointer
// survives on the stack, and whether LSan still sees it as a root is not
// stable). A one-shot leak assertion would therefore be a FLAKY oracle. The
// loop makes the report deterministic: 100 iterations → "6400 byte(s) leaked
// in 100 allocation(s)" in 20/20 runs.
//
// ── THE CONTROL ROWS ───────────────────────────────────────────────────────
// The danger of a leak fix is a DOUBLE FREE. Every escaping shape — stored in
// a field, returned, captured by another closure, passed to a parameter —
// must keep its own reference, and must NOT be released at scope end by the
// local that produced it. The control rows below are exactly those shapes;
// each asserts the CORRECT OUTPUT (the escaped value still works) and NO
// AddressSanitizer report (no double free, no use-after-free). They pass on
// the PRE-FIX tree as well: they are controls, not the regression oracle.
//
// The two controls whose programs LEAK (the field shape and the
// collection-element shape) use the LENIENT harness (`runAotCheckedLenient`:
// LeakSanitizer OFF, generated-code AddressSanitizer ON). Their leak is
// PRE-EXISTING and is a DIFFERENT defect — the escaping local's own +1 has
// no releaser because the escaping shape (a field store / a collection
// element) is a separate arm of the same walk — and arming the leak oracle on
// it would measure THAT defect instead of #8058's. The generated-code
// AddressSanitizer stays ON, so a double free is still caught.
//
// Spec: §28.9 (v0.355), §8.1, §8.5, §9.7, §26.2, §26.5, §26.7, §27.6, §28.3, §28.3.8, §28.14, stdlib §2.1
// (v0.355).

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

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when the sanitizer fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The STRICT oracle (the generated-code ASan + LSan net): the program ran, it
// printed the expected text, LSan reports ZERO leaked allocations and ZERO
// leaked bytes, and NO AddressSanitizer report of any kind is present (a
// double free would land there).
void expectNoLeak(const JitResult& r, const std::string& out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured.
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// The CONTROL oracle: the program ran, it printed the expected text, and NO
// AddressSanitizer report is present — so nothing was double-freed and
// nothing was used after being freed. The LEAK oracle is deliberately NOT
// armed here (see the header): the controls' programs legitimately leak the
// escaping local's own reference (a PRE-EXISTING, separate defect), and a
// double free is a memory-ERROR, which this oracle does catch.
void expectNoMemoryError(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// `n` copies of `line` — the expected stdout of an `n`-iteration loop.
std::string repeated(const std::string& line, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += line;
    return out;
}

// fix-forward of #8058 (§28.3.8 clause 4, v0.355): the oracle for a program
// the spec REJECTS but the compiler still ACCEPTS today. A capture-bearing
// function value at the receiver of an extension call whose head is `Object`
// is at a set-erasing position ("as the receiver of an extension call whose
// head is `Object` [...] it is", §28.3.8 clause 4), and an extension body may
// not store `this` (§26.7); neither is enforced yet (#7259, #7704). So the
// row accepts EITHER outcome the spec allows a future compiler to reach — a
// genuine compile-time rejection naming §28.3 or §26.7 — OR, while the
// program is still accepted, pins the one property THIS fix owns: the
// producing local's scope-end release must not free the closure cell while
// the extension's alias still lives (no heap-use-after-free, no double
// free). These programs ALSO crash on a PRE-EXISTING, separate defect — the
// `Object` view of a function value reached through an extension's `this`
// is misread (#7247 family; MEASURED identical on the base tree 8845ad4e:
// `System.OverflowException` or an ASan SEGV inside `bz_string_release`) —
// so a clean run is NOT asserted; the row asserts only the absence of the
// use-after-free this fix's predecessor 83c9a9cd introduced.
void expectRejectedOrNoClosureUseAfterFree(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (bezel::test::failedAtCompileTimeWithRealLocation(r)) {
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§28.3") ||
                          bezel::test::diagnosticMessageContains(r, "§26.7"));
        return;
    }
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "heap-use-after-free"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "double-free"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "attempting free on address"));
}

// fix-forward of #8058 (§26.5 rule 1 / §8.1, v0.355): the oracle for a
// program that §26.5 rule 1 REJECTS — an `extension Object` member that
// reuses an `Object` member's name ("extension blocks are **never consulted**
// for that name on that receiver") — but that the compiler still ACCEPTS on
// a function-typed receiver today (the #8051 family), and that then RUNS
// CLEANLY on the base tree. Two outcomes are admitted: a genuine
// compile-time rejection citing rule 1 (§26.5, or §8.1's arity diagnostic
// the user-type receivers already get) or the store ban (§26.7) / the
// set-erasing receiver (§28.3); OR, while the program is still accepted,
// the full control oracle — it ran, printed `out`, and NO AddressSanitizer
// report (the use-after-free the name-keyed and method-only relaxations
// introduced). Stronger than `expectRejectedOrNoClosureUseAfterFree` above
// on purpose: these programs do not hit the #7247 crash, so a crash here
// would be THIS fix's, and the row must not absorb it.
void expectRule1RejectionOrNoMemoryError(const JitResult& r,
                                         const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (bezel::test::failedAtCompileTimeWithRealLocation(r)) {
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§26.5") ||
                          bezel::test::diagnosticMessageContains(r, "§8.1") ||
                          bezel::test::diagnosticMessageContains(r, "§26.7") ||
                          bezel::test::diagnosticMessageContains(r, "§28.3"));
        return;
    }
    expectNoMemoryError(r, out);
}

}  // namespace

// ===========================================================================
// 1/9 — THE ISSUE'S OWN REPRO, LOOPED 100×. THE REGRESSION ORACLE.
//
//   var g: (int)->int := lambda(x: int) => x;
//   Console.WriteLine(g.ToString());
//
// §8.5 (v0.355): a function value renders through `Object.ToString()` as the
// stdlib §2.13 `FullName` of the type it was FORMED at (§12.7) — "(int) -> int"
// here. §28.9 (v0.355) assigns the closure CELL behind that value to the
// runtime, so the cell must be reclaimed when the local goes out of scope.
//
// MEASURED on the base tree BEFORE the fix: "SUMMARY: AddressSanitizer: 6400
//   byte(s) leaked in 100 allocation(s)" — one 64-byte cell per iteration,
//   20/20 runs.
// AFTER the fix: no leak at all, and the rendering is unchanged.
//
// The 100-iteration loop is the loop the issue's single-shot measurement
// needs (see the header): a SINGLE leaked allocation is reported only
// SOMETIMES (MEASURED: 8/20 runs on the same binary), so the loop is what
// makes the oracle deterministic.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           the_issue_repro_looped_reclaims_the_closure_cell) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i: int := 0;
    while i < 100 do
      var g: (int)->int := lambda(x: int) => x;
      Console.WriteLine(g.ToString());
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                  repeated("(int) -> int\n", 100));
}

// ===========================================================================
// 2/9 — THE ISSUE'S REPRO, VERBATIM (one iteration): the RENDERING.
//
// The same program as row 1, un-looped: the issue's own repro. This row pins
// the TEXT, not the leak count: LSan is OFF here (the LENIENT harness) because
// a SINGLE leaked allocation is only sometimes reported (MEASURED: 8/20 runs
// on the same binary), so a one-shot leak assertion would be a flaky oracle —
// row 1 carries the leak oracle, on 100 iterations.
//
// AFTER the fix: "(int) -> int" — the value's FORMED TYPE (§12.7), which is
// what §8.5 requires and what #7592's fix made the `g.ToString()` route
// produce.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           the_issue_repro_renders_the_function_values_formed_type) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x;
    Console.WriteLine(g.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "(int) -> int\n");
}

// ===========================================================================
// 3/9 — TWO FUNCTION TYPES RENDER TWO DIFFERENT FORMED TYPES.
//
// WHAT IT IS A CONTROL FOR: the fix changes how the RECEIVER of a member
// access is classified; a fix that had disturbed the rendering would show up
// here. §8.5 (v0.355) renders a function value as the stdlib §2.13 `FullName`
// of the type it was FORMED at, so "(int) -> int" and "(int, int) -> int" are
// two different texts — the row reads the DATA the fix handles, not a
// constant.
//
// Looped 100× so the strict leak oracle applies to it as well.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           two_function_types_render_two_different_formed_types) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i: int := 0;
    while i < 100 do
      var g: (int)->int := lambda(x: int) => x;
      var h: (int, int) -> int := lambda(x: int, y: int) => x + y;
      Console.WriteLine(g.ToString());
      Console.WriteLine(h.ToString());
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                  repeated("(int) -> int\n(int, int) -> int\n", 100));
}

// ===========================================================================
// 4/9 — CONTROL: a closure STORED IN A FIELD is not released at scope end.
//
// WHAT IT IS A CONTROL FOR: the field store is an ESCAPE (the field keeps its
// own reference, §28.9 / #429's field-store discipline), so the producing
// local's scope-end release must NOT fire — a release there would free the
// cell while the field still holds it (a use-after-free) or double-free it.
//
// The program proves the cell SURVIVES the producing scope: `C.Fill(h)`'s
// local `g` goes out of scope when `Fill` returns, and `h.Fn(2)` is called
// afterwards — the value must still work.
//
// The program leaks the escaping local's own reference (a PRE-EXISTING,
// separate defect: the field-store arm of the same walk), so the LEAK oracle
// is off here and the generated-code AddressSanitizer (which catches a double
// free) is on.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_closure_stored_in_a_field_is_not_released_at_scope_end) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => 0; end constructor;
end Holder;
class P
  public static method Fill(h: Holder): int
    var g: (int)->int := lambda(x: int) => x + 1;
    h.Fn := g;
    return h.Fn(1);
  end Fill;
  public static method Main(): int
    using var h := new Holder();
    Console.WriteLine(P.Fill(h).ToString());
    Console.WriteLine(h.Fn(2).ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "2\n3\n");
}

// ===========================================================================
// 5/9 — CONTROL: a RETURNED closure is not released at scope end.
//
// WHAT IT IS A CONTROL FOR: the return TRANSFERS the local's reference to the
// caller (§28.9 / §28.8), so the producing scope must not release it too —
// that would be a double free against the caller's release.
//
// The row is STRICT (leak oracle armed): this shape is LSan-clean, because
// the transferred reference is the caller's to release.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_returned_closure_is_not_released_at_scope_end) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Make(): (int)->int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g;
  end Make;
  public static method Main(): int
    var f := P.Make();
    Console.WriteLine(f(2).ToString());
    return 0;
  end Main;
end P;
)BZL"),
                  "3\n");
}

// ===========================================================================
// 6/9 — CONTROL: a closure CAPTURED BY ANOTHER CLOSURE is not released at
// scope end.
//
// WHAT IT IS A CONTROL FOR: a captured binding is a BORROW whose source is the
// capturing local's enclosing scope (§28.9), and the capturing closure's
// env-disposer owns the cell's release. A scope-end release of the captured
// local's slot would double-free the cell against the env-disposer.
//
// The row is STRICT (leak oracle armed): this shape is LSan-clean.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_closure_captured_by_another_closure_is_not_released_at_scope_end) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x + 1;
    var h := lambda() => g(2);
    Console.WriteLine(h().ToString());
    return 0;
  end Main;
end P;
)BZL"),
                  "3\n");
}

// ===========================================================================
// 7/9 — CONTROL: a closure PASSED TO A PARAMETER is not released at scope end.
//
// WHAT IT IS A CONTROL FOR: the "passed to a parameter" escaping shape. §28.14
// (v0.355) makes a plain (un-`scoped`) function-typed parameter CALL-BOUNDED —
// "a plain function-typed parameter may be invoked, bound to locals, and
// passed to another plain function-typed parameter, but may not be stored in a
// field of any object" — and SEMA enforces that independently of codegen (the
// compiler REJECTS `this.F := f` for such a formal; row 8 pins that
// rejection). So the reachable "parameter" shape is the call-bounded one: the
// callee may INVOKE the closure but cannot store it, so the caller's local
// keeps its own reference and must release it at scope end — exactly the
// discipline #5611 installed.
//
// The callee invoking the closure must not consume the caller's reference
// either, so the row also pins that the closure still works after the call.
//
// The row is STRICT (leak oracle armed): this shape is LSan-clean.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_closure_passed_to_a_call_bounded_parameter_keeps_its_reference) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Apply(f: (int)->int): int
    return f(2);
  end Apply;
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x + 1;
    Console.WriteLine(P.Apply(g).ToString());
    return 0;
  end Main;
end P;
)BZL"),
                  "3\n");
}

// ===========================================================================
// 8/9 — CONTROL: the "storing parameter" escaping shape is NOT REACHABLE.
//
// WHAT IT IS A CONTROL FOR: §28.14 (v0.355) rejects storing a plain
// function-typed PARAMETER into a field — "parameter `f` is call-bounded;
// declare it `scoped` to store it (§28.14)". So the fourth escaping shape the
// fix had to consider (passed to a STORING parameter) cannot arise for a
// plain parameter; row 7 covers the reachable one.
//
// This row pins the REJECTION, so a future change that made the storing shape
// reachable would have to add a control for it (the row would then fail).
//
// It is a compile-time REJECTION row (the program never runs), so it is not a
// memory-safety observation and carries no leak oracle.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_plain_function_typed_parameter_may_not_be_stored_in_a_field) {
    const JitResult r = runAot(R"BZL(
module M;
class Sink
  public F: (int)->int;
  public constructor(f: (int)->int) this.F := f; end constructor;
end Sink;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x + 1;
    using var s := new Sink(g);
    Console.WriteLine(s.F(2).ToString());
    return 0;
  end Main;
end P;
)BZL");
    // The program must be REJECTED BY THE COMPILER at a real source location
    // (never a crash, a signal, a timeout or an ASan report — `r.ran` alone
    // cannot tell those apart, #3892), and the diagnostic must be §28.14's
    // call-bounded-parameter rule — the rule that makes the storing shape
    // unreachable.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "is call-bounded"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§28.14"));
}

// ===========================================================================
// 9/9 — CONTROL: a closure local with NO member access still reclaims its
// cell (the pre-existing behaviour is unchanged).
//
// WHAT IT IS A CONTROL FOR: the fix must not CHANGE the arms it does not
// touch. This is the shape the scope-end release was armed for BEFORE the
// fix (MEASURED on the base tree: `bz_closure_release` referenced), so the
// row pins that it is still armed — and that it fires exactly once per
// iteration (no double free).
//
// Looped 100× so the strict leak oracle is deterministic (see the header).
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_closure_local_without_a_member_access_still_reclaims_its_cell) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i: int := 0;
    while i < 100 do
      var g: (int)->int := lambda(x: int) => x;
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                  "");
}

// ###########################################################################
// FIX-FORWARD ROWS (review of 83c9a9cd). 83c9a9cd relaxed the receiver of
// EVERY callee-position member access; the relaxation is sound only for a
// member `Object` itself DECLARES (§8.1: GetType, Equals, GetHashCode,
// ToString). An EXTENSION call whose head is `Object` — which §26.5 rule 2
// reaches from a function-typed receiver — runs USER code that "may pass
// [`this`] on to a parameter that stores it" (spec §9.7, v0.355), and
// §28.3.8 clause 4 (v0.355) names that receiver set-erasing. Arming the
// scope-end release there freed the cell while the extension's alias lived
// (MEASURED at 83c9a9cd: heap-use-after-free, freed by `bz_closure_release`).
// ###########################################################################

// ===========================================================================
// FIX-FORWARD 1 — the review's r24: an `extension Object` member returns
// `this` (non-owned, which §26.7 permits) and the caller STORES the result
// in an `Array<Object>`. The function value is CAPTURE-FREE, so §28.3.8's
// set-erasing check admits it: the program is LEGAL. The stored alias is the
// closure cell itself, so the producing local must NOT release the cell at
// scope end.
//
// The final read is `o.GetHashCode()` rather than the review probe's
// `o.ToString()`: `ToString()` on that Object view crashes on a PRE-EXISTING,
// separate defect (#7247 family — MEASURED identical on the base tree
// 8845ad4e and on this fix: ASan SEGV inside `bz_string_release`).
// `GetHashCode()` reads the stored object, so a freed cell is caught.
//
// MEASURED at 83c9a9cd: heap-use-after-free (freed by `bz_closure_release`).
// AFTER the fix-forward: "100", no AddressSanitizer report.
// The LEAK oracle is off (lenient harness): the escaped cells are never
// released by anything (MEASURED: 97–98 × 64 B — the conservative escape's
// PRE-EXISTING leak, the same code path as the base tree's for a non-`Object`
// member name), a separate defect from the use-after-free this row pins.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           an_extension_object_self_result_stored_keeps_the_closure_cell_alive) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method Self(): Object => this;
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    xs.Add(g.Self());
    return 0;
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

// ===========================================================================
// FIX-FORWARD 2 — the review's r18, capture-free: `return g.Self();` hands
// the closure cell itself to the caller through an `Object`-typed return.
// The producing local must not release it at scope end; the caller reads it.
//
// MEASURED at 83c9a9cd: heap-use-after-free. AFTER: "100", no ASan report.
// Lenient harness: the returned Object view's +1 is never released
// (MEASURED 100 × 64 B — pre-existing, the conservative escape's leak).
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           an_extension_object_self_result_returned_keeps_the_closure_cell_alive) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method Self(): Object => this;
end;
class P
  public static method Make(): Object
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.Self();
  end Make;
  public static method Main(): int
    var i: int := 0;
    var n: int := 0;
    while i < 100 do
      var o := P.Make();
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

// ===========================================================================
// FIX-FORWARD 3 — the review's r25: the extension BODY stores `this`
// (`xs.Add(this)`). §26.7 (v0.355) forbids it ("extensions cannot store
// `this`") but the compiler does not enforce it yet (#7704). So a
// compile-time rejection naming §26.7 is accepted; while the program is
// still accepted it must not read a freed cell.
//
// MEASURED at 83c9a9cd: heap-use-after-free. AFTER: "100", no ASan report.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           an_extension_object_body_storing_this_keeps_the_closure_cell_alive) {
    const JitResult r = runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method KeepIn(xs: Array<Object>): int
    xs.Add(this);
    return 0;
  end KeepIn;
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.KeepIn(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL");
    if (bezel::test::failedAtCompileTimeWithRealLocation(r)) {
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§26.7"));
    } else {
        expectNoMemoryError(r, "100\n");
    }
}

// ===========================================================================
// FIX-FORWARD — the review's r19 (`xs.Add(g.Self())`, capture-bearing), verbatim.
// The program is one §28.3.8 clause 4 rejects (#7259 / #7704 open), and it
// crashes on the pre-existing Object-view defect on the base tree too
// (MEASURED 8845ad4e and this fix: `System.OverflowException`, exit 70). See
// `expectRejectedOrNoClosureUseAfterFree`.
//
// MEASURED at 83c9a9cd: heap-use-after-free, freed by `bz_closure_release`.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_capturing_closure_stored_through_an_extension_object_self_is_not_freed_early) {
    expectRejectedOrNoClosureUseAfterFree(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method Self(): Object => this;
end;
class P
  public static method Run(xs: Array<Object>, k: int): int
    var g: (int)->int := lambda(x: int) => x + k;
    xs.Add(g.Self());
    return 0;
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs, i);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do n := n + o.ToString().Length; end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"));
}

// ===========================================================================
// FIX-FORWARD — the review's r18 (`return g.Self();`, capture-bearing), verbatim.
// The program is one §28.3.8 clause 4 rejects (#7259 / #7704 open), and it
// crashes on the pre-existing Object-view defect on the base tree too
// (MEASURED 8845ad4e and this fix: `System.OverflowException`, exit 70). See
// `expectRejectedOrNoClosureUseAfterFree`.
//
// MEASURED at 83c9a9cd: heap-use-after-free, freed by `bz_closure_release`.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_capturing_closure_returned_through_an_extension_object_self_is_not_freed_early) {
    expectRejectedOrNoClosureUseAfterFree(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method Self(): Object => this;
end;
class P
  public static method Make(k: int): Object
    var g: (int)->int := lambda(x: int) => x + k;
    return g.Self();
  end Make;
  public static method Main(): int
    var i: int := 0;
    var n: int := 0;
    while i < 100 do
      var o := P.Make(i);
      n := n + o.ToString().Length;
      i := i + 1;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"));
}

// ===========================================================================
// FIX-FORWARD — the review's r12 (extension body `xs.Add(this)`, capture-bearing), verbatim.
// The program is one §28.3.8 clause 4 rejects (#7259 / #7704 open), and it
// crashes on the pre-existing Object-view defect on the base tree too
// (MEASURED 8845ad4e and this fix: `System.OverflowException`, exit 70). See
// `expectRejectedOrNoClosureUseAfterFree`.
//
// MEASURED at 83c9a9cd: heap-use-after-free, freed by `bz_closure_release`.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_capturing_closure_stored_by_an_extension_object_body_is_not_freed_early) {
    expectRejectedOrNoClosureUseAfterFree(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method KeepIn(xs: Array<Object>): int
    xs.Add(this);
    return 0;
  end KeepIn;
end;
class P
  public static method Run(xs: Array<Object>, k: int): int
    var g: (int)->int := lambda(x: int) => x + k;
    return g.KeepIn(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs, i);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do n := n + o.ToString().Length; end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"));
}

// ###########################################################################
// FIX-FORWARD CONTROLS: `g.ToString()` (a member `Object` DECLARES — the
// relaxed position) COMBINED with each escaping shape. The relaxation removes
// ONE escape source; each of the shapes below must still keep the local
// escaped, or the scope-end release frees a cell that is still referenced.
// Each asserts the exact output and no AddressSanitizer report. The leak
// oracle is armed only where the shape is LSan-clean (the return); the
// field / capture / collection shapes keep PRE-EXISTING leaks of the escaped
// reference (MEASURED identical on 8845ad4e, 83c9a9cd and this fix:
// r01 7600 B / 300, r03 400 B / 100, r04 7448 B / 294 — the review's matrix).
// ###########################################################################

// ===========================================================================
// CONTROL — the review's r01: `g.ToString()` then `h.Fn := g;`; the field is
// called after the producing scope ends.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           to_string_then_a_field_store_keeps_the_closure_cell_alive) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => 0; end constructor;
end Holder;
class P
  public static method Fill(h: Holder, k: int): string
    var g: (int)->int := lambda(x: int) => x + k;
    var s := g.ToString();
    h.Fn := g;
    return s;
  end Fill;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do
      var s := P.Fill(h, i);
      total := total + h.Fn(1) + s.Length;
      i := i + 1;
    end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "6250\n");
}

// ===========================================================================
// CONTROL — the review's r02: `g.ToString()` then `return g;`. STRICT: the
// shape is LSan-clean (the caller releases the transferred reference).
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           to_string_then_a_return_keeps_the_closure_cell_alive) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Make(k: int): (int)->int
    var g: (int)->int := lambda(x: int) => x + k;
    Console.WriteLine(g.ToString());
    return g;
  end Make;
  public static method Main(): int
    var i: int := 0;
    var total: int := 0;
    while i < 100 do
      var f := P.Make(i);
      total := total + f(1);
      i := i + 1;
    end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 repeated("(int) -> int\n", 100) + "5050\n");
}

// ===========================================================================
// CONTROL — the review's r03: `g.ToString()` then `g` captured by a
// returned lambda, which calls it after the producing scope ends.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           to_string_then_a_capture_keeps_the_closure_cell_alive) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Make(k: int): () -> int
    var g: (int)->int := lambda(x: int) => x + k;
    var s := g.ToString();
    var h := lambda() => g(s.Length);
    return h;
  end Make;
  public static method Main(): int
    var i: int := 0;
    var total: int := 0;
    while i < 100 do
      var f := P.Make(i);
      total := total + f();
      i := i + 1;
    end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "6150\n");
}

// ===========================================================================
// CONTROL — the review's r04: `g.ToString()` then `xs.Add(g)`; every
// stored element is called after the producing scopes end.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           to_string_then_a_collection_add_keeps_the_closure_cell_alive) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Fill(xs: Array<(int)->int>, k: int): string
    var g: (int)->int := lambda(x: int) => x + k;
    var s := g.ToString();
    xs.Add(g);
    return s;
  end Fill;
  public static method Main(): int
    var xs := new Array<(int)->int>();
    var i: int := 0;
    var n: int := 0;
    while i < 100 do
      n := n + P.Fill(xs, i).Length;
      i := i + 1;
    end;
    var total: int := 0;
    for var f in xs do
      total := total + f(1);
    end;
    Console.WriteLine(total.ToString() + " " + n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "5050 1200\n");
}

// ===========================================================================
// POSITIVE — the relaxation covers EVERY member `Object` declares (§8.1),
// not only `ToString`: `g.GetHashCode()` is §28.3.8 clause 4's own example
// of a non-set-erasing receiver. STRICT: no leaked cell, looped 100×.
// MEASURED on this fix: "100", LSan-clean.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           get_hash_code_on_a_closure_local_reclaims_the_closure_cell) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i: int := 0;
    var n: int := 0;
    while i < 100 do
      var g: (int)->int := lambda(x: int) => x;
      if g.GetHashCode() = g.GetHashCode() then n := n + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "100\n");
}

// ===========================================================================
// SAME-NAMED EXTENSION — an `extension Object` member that REUSES an `Object`
// member's NAME at another arity. §26.5 rule 1 (v0.355) says extension
// blocks are "never consulted for that name on that receiver", but sema does
// not enforce the rule on every receiver (#8051: the primitive-receiver
// hole), so the call BINDS to the extension and user code runs on the
// receiver — `xs.Add(this)` / `=> this`, exactly the alias §9.7 (v0.355)
// warns of: "the extension body reads `this` at the head type and may pass
// it on to a parameter that stores it". The receiver is therefore §28.3.8
// clause 4's set-erasing "receiver of an extension call whose head is
// `Object`", and the closure local must NOT release its cell at scope end.
//
// MEASURED at eb91cd7a (the name-keyed relaxation), each row's program via
// `scripts/agent_check.sh --probe --asan`: `ERROR: AddressSanitizer:
// heap-use-after-free`, freed by `bz_closure_release`. On the base tree
// 8845ad4e and on this fix: "100", no AddressSanitizer report.
//
// THESE PROGRAMS ARE ILLEGAL under §26.5 rule 1 (and the first two under
// §26.7's "extensions cannot store `this`"); the compiler does not enforce
// either on a function-typed receiver yet. The oracle is therefore
// `expectRule1RejectionOrNoMemoryError`: a rule-1 rejection is accepted, and
// until one exists the row pins exact output "100" and no AddressSanitizer
// report — never an admission.
//
// Lenient harness (LeakSanitizer OFF): the escaped cells have no releaser —
// the conservative escape's PRE-EXISTING leak (MEASURED on base: 97–99 × 64
// B), a separate defect from the use-after-free these rows pin. The final
// read is `o.GetHashCode()`, which dereferences the stored cell (a freed one
// is caught); `o.ToString()` on that Object view crashes on the pre-existing
// #7247-family defect and would not reach the oracle.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_to_string_storing_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method ToString(xs: Array<Object>): string
    xs.Add(this);
    return "x";
  end ToString;
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.ToString(xs).Length;
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_equals_storing_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method Equals(xs: Array<Object>, k: int): bool
    xs.Add(this);
    return true;
  end Equals;
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return (if g.Equals(xs, 1) then 1 else 0);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_get_hash_code_returning_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
extension Object
  public method GetHashCode(tag: string): Object => this;
end;
class P
  public static method Make(): Object
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.GetHashCode("t");
  end Make;
  public static method Main(): int
    var i: int := 0;
    var n: int := 0;
    while i < 100 do
      var o := P.Make();
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

// ===========================================================================
// SAME-NAMED EXTENSION PROPERTY — the third spelling. §26.3 (v0.355) admits
// "computed read-only properties" in an extension block, so an `extension
// Object` PROPERTY of function type that reuses an `Object` member's name
// (`public property ToString: (Array<Object>) -> int => lambda(xs) =>
// Keep.Take(xs, this);`) is a member too, and `g.ToString(xs)` on a
// function-typed receiver binds it (§26.5 rule 1 is not enforced there, as
// above). Sema stamps a PropertySymbol, not a MethodSymbol with
// `extensionDecl`, so the previous fix-forward (6f5448ff) — which asked only
// "is this an extension METHOD?" — still relaxed the receiver, and the
// lambda's alias of `this` (stored by `Keep.Take`) outlived the cell.
//
// MEASURED at 6f5448ff (`scripts/agent_check.sh --probe --asan`, 100x):
// ToString / GetType: `ERROR: AddressSanitizer: heap-use-after-free`, freed
// by `bz_closure_release`; GetHashCode: 92 cells fewer leaked than base,
// i.e. freed under a live alias. On base 8845ad4e and on this fix: "100",
// no AddressSanitizer report. The walker now relaxes only a callee node
// that carries NO resolved symbol at all.
//
// Same oracle as the method rows (`expectRule1RejectionOrNoMemoryError`):
// a rule-1 rejection is accepted; until one exists, exact output and no
// ASan report. Lenient harness: the escaped cells and the per-call lambda
// cells leak on every arm (MEASURED on base: 292 allocations), the
// conservative escape's pre-existing leak.
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_property_to_string_storing_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Keep
  public static method Take(xs: Array<Object>, o: Object): int
    xs.Add(o);
    return 0;
  end Take;
end Keep;
extension Object
  public property ToString: (Array<Object>) -> int => lambda(xs: Array<Object>) => Keep.Take(xs, this);
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.ToString(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                                        "100\n");
}

BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_property_get_type_storing_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Keep
  public static method Take(xs: Array<Object>, o: Object): int
    xs.Add(o);
    return 0;
  end Take;
end Keep;
extension Object
  public property GetType: (Array<Object>) -> int => lambda(xs: Array<Object>) => Keep.Take(xs, this);
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.GetType(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                                        "100\n");
}

// The `GetHashCode`-named property hides `Object.GetHashCode()` on EVERY
// `Object`-typed receiver in the module (rule 1 is unenforced for a property
// even there — a pre-existing sema gap the review recorded), so the final
// read cannot be a direct `o.GetHashCode()`. MEASURED on this fix for the
// other candidates: `o.Equals(o)` is a pointer comparison that reads nothing
// (the review's x13 printed 100 on 6f5448ff although 92 cells were freed
// under the alias); `o.ToString()` on this view is the pre-existing
// #7247-family crash; `o.GetType()` is unimplemented (#487); the §12.7
// type test `o is (int) -> int` reads nothing freed either (and is `false`
// for the view an `extension Object` body's `this` stores — a separate
// observation, reported with this fix). The read is therefore
// `Object.GetHashCode()` reached through a GENERIC body, `Keep.H<Object>(o)`
// with `method H<T>(x: T): int => x.GetHashCode()`, where the hiding
// property is not in the lookup: on 6f5448ff that read is `ERROR:
// AddressSanitizer: heap-use-after-free` (freed by `bz_closure_release`,
// MEASURED via `--probe --asan`); on this fix "100", no ASan report.
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_same_named_extension_property_get_hash_code_storing_this_keeps_the_closure_cell_alive) {
    expectRule1RejectionOrNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Keep
  public static method Take(xs: Array<Object>, o: Object): int
    xs.Add(o);
    return 0;
  end Take;
  public static method H<T>(x: T): int => x.GetHashCode();
end Keep;
extension Object
  public property GetHashCode: (Array<Object>) -> int => lambda(xs: Array<Object>) => Keep.Take(xs, this);
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.GetHashCode(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if Keep.H<Object>(o) = Keep.H<Object>(o) then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                                        "100\n");
}

// CONTROL — the same property under a name `Object` does not declare
// (`Hook`). Never relaxed on any arm (not a §8.1 name), so it is identical
// on base, on every fix, and here: "100", no ASan report. It shows the
// property rows above fail only through the relaxation.
BEZEL_TEST(closure_cell_member_access_release_8058,
           an_extension_property_under_another_name_is_a_control) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class Keep
  public static method Take(xs: Array<Object>, o: Object): int
    xs.Add(o);
    return 0;
  end Take;
end Keep;
extension Object
  public property Hook: (Array<Object>) -> int => lambda(xs: Array<Object>) => Keep.Take(xs, this);
end;
class P
  public static method Run(xs: Array<Object>): int
    var g: (int)->int := lambda(x: int) => x + 1;
    return g.Hook(xs);
  end Run;
  public static method Main(): int
    var xs := new Array<Object>();
    var i: int := 0;
    while i < 100 do
      _ := P.Run(xs);
      i := i + 1;
    end;
    var n: int := 0;
    for var o in xs do
      if o.GetHashCode() = o.GetHashCode() then n := n + 1; end;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "100\n");
}

// ===========================================================================
// 28/9 — a local bound from an EXPRESSION-BODIED GETTER's +1, then rendered.
//
//   class Holder … public method Get(): (int)->int => this.Fn;
//   var g: (int)->int := h.Get();  var s := g.ToString();
//
// The third review's blocker (rev-8058d BLOCKING 1): this program was a
// heap-use-after-free on the unit's first base, because an arrow getter
// handed its caller a +0 borrow that the caller then released (#8141), and
// this unit's relaxation of the `g.ToString()` receiver made the member-call
// spellings reach that over-release. #8141 now mints the +1 in the arrow
// getter's return (§22.2, §28.9, v0.355), exactly as the block twin
// `return this.Fn;` does, so the getter's result is the caller's own
// reference, and this unit's scope-end `bz_closure_release` of `g` is the
// drop of THAT reference, never of the field's.
//
// MEASURED, 100x, `agent_check.sh --probe --asan`:
//   r30-c1 alone (#8141 in, unit out): "2000", NO ASan error, 72-byte /
//     2-allocation LEAK (the cell and its env block) — the local keeps the
//     +1 it was handed, because the `g.ToString()` receiver is still an
//     escape (the cell's count never returns to the field's one, so
//     `using var h` cannot free it);
//   r30-c1 + this unit: "2000", NO leak, NO ASan error.
// So the row is RED without the unit (the leak) and RED on the unit as first
// reviewed, 65defd9d without #8141 (the use-after-free), and GREEN only on
// the combined tree: it pins
// both that the cell is reclaimed AND that reclaiming it is not a double
// free of the field's reference (§28.9 v0.355: "Runtime management governs
// the *function value itself*").
// ===========================================================================
BEZEL_TEST(closure_cell_member_access_release_8058,
           a_local_bound_from_an_arrow_getter_then_rendered_reclaims_only_its_own_reference) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; end constructor;
  public method Get(): (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get();
    var s := g.ToString();
    return s.Length;
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "2000\n");
}

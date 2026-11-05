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
// WHY THE RECEIVER MAY BE RELEASED AT SCOPE END. §26.2 (v0.355) closes the
// extendable list against function types ("What cannot be extended: […]
// Function types"), so a function value's member surface is exactly
// `Object`'s — §8.5's `ToString` rendering and §8.2's `GetType` — and BOTH
// are compiler-provided intrinsics: no user code runs on the receiver, so the
// call cannot store it anywhere that outlives the call. The receiver's own
// reference stays in the receiver's slot, and the slot's scope-end release is
// therefore exactly-once and free of any dangling alias.
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
// Spec: §28.9 (v0.355), §8.1, §8.5, §26.2, §27.6, §28.3, §28.14, stdlib §2.1
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
    // The program must be REJECTED (it never runs), and the diagnostic must
    // name §28.14 — the rule that makes the storing shape unreachable.
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("§28.14") != std::string::npos);
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

// Regression tests for #8137 -- a PLAIN `var c := t.Result` (and the `?.`
// spelling `var c := o?.Result`) on an `OwningTask<Cell>` LEAKED the
// transferred result.
//
// THE DEFECT (MEASURED, on the tree that carries #8131's dd45bbe2).
//   using var t := Mk();            // OwningTask<Cell>
//   var o: OwningTask<Cell>? := t;
//   var c := o?.Result;              // <-- the plain `var` spelling
// reads the RIGHT value (the `?.` spelling was taught by #7896/#7981, and the
// transfer really happens -- the task's result buffer is consumed), but nobody
// ever DISPOSES the transferred class object: 16 bytes per read, so 100
// allocations / 1600 bytes over a 100x loop under ASan/LSan, and the class's
// live counter stands at 100 where §28.3 requires 0. There is no double free.
//
// ROOT CAUSE. The ownership pass classified the `.Result` / `?.Result` read as
// a BORROW (`MethodOwnership::producerState`, src/ownership/Ownership.cpp), so
// the plain binding was never OWNED, no §28.3 affine auto-drop was ever
// flagged (`ast::LocalDecl::autoDropAtScopeEnd`), and codegen never
// registered a scope-end disposal for it. The `using var` spelling was
// already discharged -- codegen's `usingVarOwnsClassObjectDynamically` case
// (b0d) registers the scope-end disposal for it -- which is exactly why the
// defect was spelling-dependent.
//
// THE SPEC (language spec v0.355).
//
//   §30.3 (v0.355), `OwningTask<T>`: "reading it on a completed task in an
//   owned-consuming position transfers ownership of the result to the reader
//   exactly as `await` does -- the result buffer is consumed, and a second
//   read or `await` raises `InvalidOperationException`." A local binding IS an
//   owned-consuming position (the consume table's binding row), so the plain
//   local owns the transferred value.
//
//   §28.3 (v0.355): an owned local is discharged at scope end -- the affine
//   auto-drop -- and the §28.3 must-use warning tells the programmer to write
//   `using var` for the explicit form.
//
//   §7.5 (v0.355): "`x?.M` returns null if `x` is null; otherwise it performs
//   the access." So `o?.Result` performs the very access `o.Result` performs
//   -- §30.3's transfer included -- and yields the NULLABLE of the member
//   type, so the slot holds a null pointer when the receiver is null.
//
// THE ORACLE. Every row loops 100 times (a one-shot leak repro is a vacuous
// oracle) and asserts BOTH that the class's live counter is back to zero (the
// destructor ran EXACTLY once) and that LeakSanitizer reports no leak. The
// rows run under `runAotChecked`, the ASan/LSan oracle, so the leak assertion
// is live in a NORMAL suite run and not only under the
// `BEZEL_AOT_FORCE_SANITIZE=address` sweep.
//
// THE CONTROL rows. The `using var` spelling (already clean), the NULL
// receiver (the `?.` short-circuits, nothing is transferred, and the disposal
// must be SKIPPED rather than firing on a null slot), the ANNOTATED spelling
// (the declared type names the class, so the static class-disposal path
// reaches the binding), and the VALUE-typed `Task<int>?` receiver (no
// disposal obligation at all -- §28.9).
//
// THE FIX-FORWARD ROWS (added by the fix-forward of #8137). #8137 made the
// plain binding OWN the transferred result and taught codegen the SCOPE-END
// disposal for it, but §28.7's OTHER half -- the drop-on-overwrite -- was
// still missing: codegen's `disposeDisplacedOwnedClassLocal` gates on
// `unwindOwnedLocalClass`, which classifies a local by its INITIALIZER FORM
// (`new C()` / an `owned`-returning call / ...) and did not know the §30.3
// owned `Result`-read form. So REASSIGNING such a local
//
//   var c := t.Result;      // owns the transferred Cell
//   c := new Cell(7);       // §28.7: the DISPLACED Cell must be disposed
//
// leaked the displaced value: MEASURED on 555708b9 (build/bezelc
// --sanitize=address, 100 iterations), 1600 bytes in 100 allocations, and the
// class's live counter standing at 100. The fix-forward teaches the
// drop-on-overwrite gate the SAME shared fact the ownership pass reads
// (`common::owningTaskResultReadOwnsClassResult`), so the classification and
// the emitted disposal cannot drift.
//
// §28.7 (v0.355): "Assignment to an owned field or owned local disposes the
// displaced value ... if the previous value is live ... it is disposed; then
// the new value is stored."

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program under test. `Cell` is a class with a destructor and a static
// live counter, so "the destructor ran exactly once" is directly observable;
// `Mk` is an `async` method returning `OwningTask<Cell>`, the §30.5 Pattern B
// producer. `body` is the loop body; the scaffolding loops 100 times and
// prints the accumulated sum and the final live count.
std::string prog(const std::string& body) {
    return
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Mk(): OwningTask<Cell> return new Cell(5); end Mk; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        + body +
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    Console.WriteLine($\"live {Cell.n}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The leak oracle. `r.exitCode` is 99 when the sanitizer fired (the harness
// runs the program with `exitcode=99`), so the exit-code assertion alone
// already turns a leak RED; the headline assertions below cover the case
// where the sanitizer report is truncated before its SUMMARY line, and the
// parsed counts are the numeric form of the same observation when the SUMMARY
// line survives.
void expectNoLeak(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocs, 0ull);
    BEZEL_EXPECT_EQ(leakedBytes, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("SUMMARY: AddressSanitizer") == std::string::npos);
}

}  // namespace

// §30.3 / §28.3 -- THE ISSUE'S REPRO, looped, in the `?.` spelling. The
// ownership transfer happens, so the plain local owns the class object and
// §28.3 auto-drops it at scope end. Pre-fix: 100 allocations / 1600 bytes
// leaked and the live counter stood at 100.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_nullcond_result_disposes_transferred_result_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      var c := o?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §30.3 / §28.3 -- the `.` spelling, the other half of the issue's title. The
// transfer is the same and the discharge must be the same.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_disposes_transferred_result_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      s := s + c.v; "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §7.5 -- the NULL receiver. `o?.Result` short-circuits: nothing is read,
// nothing is transferred, and the binding holds null. The scope-end disposal
// must be SKIPPED (a null slot), not fired on a null pointer. This row is
// GREEN before the fix too -- it is the control that says the fix does not
// make the null path dispose anything.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_nullcond_result_over_a_null_receiver_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "      var on: OwningTask<Cell>? := null; "
        "      var c := on?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "0\nlive 0\n");
}

// §30.3 / §28.3 -- the ANNOTATED `?.` spelling: `var c: Cell? := o?.Result`.
// The declared type names the class, so the class-disposal path reaches this
// binding; the row pins that the fix discharges it exactly once (not twice).
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_annotated_nullcond_result_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      var c: Cell? := o?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §30.3 / §28.3 -- the ANNOTATED `.` spelling: `var c: Cell := t.Result`.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_annotated_dot_result_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c: Cell := t.Result; "
        "      s := s + c.v; "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §28.10 -- the `using var` spelling, the CONTROL. This shape was already
// clean (codegen's case (b0d) recognised it) and must stay clean: the fix
// must not disturb it, and in particular must not double-dispose it.
BEZEL_TEST(owning_task_result_plain_var_8137,
           using_var_result_control_is_unchanged_spec_28_10) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      using var c := t.Result; "
        "      s := s + c.v; "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §28.9 / §30.3 -- the VALUE-typed control: a `Task<int>?` receiver's
// `Result` is an `int`, which carries no disposal obligation, so no disposal
// may be registered for it (a registered one would dispose a non-heap value).
// The row pins the value (700 = 7 x 100) and the absence of any sanitizer
// finding.
BEZEL_TEST(owning_task_result_plain_var_8137,
           value_typed_task_result_carries_no_disposal_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class C "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t0 := Task.FromResult(7); "
        "      var t: Task<int>? := t0; "
        "      var c := t?.Result; "
        "      s := s + (c ?? 0); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectNoLeak(r, "700\n");
}

// §28.3 -- the DISCHARGE spelling: `_ := t.Result;`. The §17.13 discard is an
// owned-consuming position and disposes the transferred value at the end of
// the statement, so the live counter returns to 0 with no binding at all.
BEZEL_TEST(owning_task_result_plain_var_8137,
           discard_result_disposes_transferred_result_spec_17_13) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      _ := t.Result; "));
    expectNoLeak(r, "0\nlive 0\n");
}

// =====================================================================
// THE FIX-FORWARD ROWS -- §28.7's DROP-ON-OVERWRITE for a plain `var`
// bound to an `OwningTask`'s `Result`.
//
// Each row REASSIGNS the local that owns the transferred result. §28.7:
// "Assignment to an owned field or owned local disposes the displaced value
// ... if the previous value is live ... it is disposed; then the new value is
// stored." The displaced value is the transferred Cell; the new value is
// disposed at scope end by the §28.3 auto-drop. So BOTH are disposed exactly
// once -- the live counter returns to 0 and LeakSanitizer reports nothing.
//
// PRE-FIX (555708b9): 1600 bytes leaked in 100 allocations, live counter 100.
// The rows below FAIL on that arm (the leak assertion and the live counter
// both go red) and PASS with the fix-forward.
// =====================================================================

// §28.7 -- the `.` spelling: `var c := t.Result; c := new Cell(7);`.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_reassign_disposes_displaced_value_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      s := s + c.v; "
        "      c := new Cell(7); "
        "      s := s + c.v; "));
    expectNoLeak(r, "1200\nlive 0\n");
}

// §28.7 -- the ANNOTATED `.` spelling: `var c: Cell := t.Result;`. The
// declared type names the class; the disposal obligation and the displaced
// value are the same as in the unannotated spelling.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_annotated_dot_result_reassign_displaces_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c: Cell := t.Result; "
        "      s := s + c.v; "
        "      c := new Cell(7); "
        "      s := s + c.v; "));
    expectNoLeak(r, "1200\nlive 0\n");
}

// §7.5 / §28.7 -- the `?.` spelling: `var c := o?.Result; c := new Cell(7);`.
// The `?.` read yields the NULLABLE of the member type, so the slot may hold
// null -- over a PRESENT receiver it holds the transferred Cell, and that is
// the displaced value §28.7 disposes.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_nullcond_result_reassign_displaces_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      var c := o?.Result; "
        "      s := s + (c?.v ?? 0); "
        "      c := new Cell(7); "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "1200\nlive 0\n");
}

// §28.7 / §28.3 -- REASSIGN-THEN-SCOPE-END: the reassignment is the last
// statement of the binding's scope, so the new value is dead at scope end and
// the §28.3 auto-drop disposes it there. The displaced (transferred) value
// was already disposed at the overwrite -- exactly once each.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_reassign_then_scope_end_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      s := s + c.v; "
        "      c := new Cell(7); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §28.7 / §30.3 -- the displaced value is ANOTHER TRANSFER: `c := t2.Result;`.
// The first read's transferred Cell is the displaced value (disposed at the
// overwrite); the second read's transferred Cell is the new value (disposed at
// scope end). Two DIFFERENT tasks, so the second read is not the §30.3
// "second read" that raises InvalidOperationException.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_reassign_from_another_result_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      using var t2 := Mk(); "
        "      var c := t.Result; "
        "      s := s + c.v; "
        "      c := t2.Result; "
        "      s := s + c.v; "));
    expectNoLeak(r, "1000\nlive 0\n");
}

// §28.7 -- the CONTROL: the SAME reassignment shape on a NORMAL owned local
// (`var c := new Cell(5); c := new Cell(7);`). It was already clean (the
// drop-on-overwrite gate admits the `new`-initializer form), so the fix
// must leave it exactly as it was: the displaced Cell disposed at the
// overwrite, the new one at scope end, live counter 0, no leak.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_reassign_of_a_normal_owned_local_control_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      var c := new Cell(5); "
        "      s := s + c.v; "
        "      c := new Cell(7); "
        "      s := s + c.v; "));
    expectNoLeak(r, "1200\nlive 0\n");
}

// §21.1 -- the `const` spelling of the same read is a COMPILE ERROR, and this
// row pins that diagnostic.
//
// A runtime `const` local "must not take an owned production" (§21.1, v0.355)
// -- the binding never takes a disposal obligation, so an owned production
// bound to it would leak. The `Result` read of an `OwningTask` IS an owned
// production (§30.3's transfer), so `const c := t.Result;` is rejected.
//
// This is the flip #8137 made and did not name: before #8137 the read was a
// BORROW and the `const` was accepted (and leaked). The diagnostic is the
// generic §21.1 wording, which names the `using var` remedy. The row is
// GREEN on 555708b9 (it pins the #8137 behaviour) and stays green with the
// fix-forward -- the fix-forward does not touch the `const` path.
//
// The assertion is on the SPECIFIC message (the §21.1 rule, the local's
// name, and the `using var` remedy), not merely on "some error", and the
// program must have been REJECTED (it never ran) -- `runAot` fills the
// diagnostics only on a reject, so this is the live oracle for the row.
BEZEL_TEST(owning_task_result_plain_var_8137,
           const_bind_of_owned_task_result_is_rejected_spec_21_1) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      const c := t.Result; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // rejected at compile time, with a real source location
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find(
        "a `const` local (`c`) must not take an owned production") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("bind owned values with `using var`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("§21.1 / §28.3") != std::string::npos);
}

// §28.7 / §28.3 -- the GENERIC body, the second fix-forward's row: codegen's
// drop-on-overwrite gate must never classify a local OWNED that the
// ownership pass classifies as a BORROW.
//
// `G<T>(t: OwningTask<T>, b1: T, b2: T) where T: class` reads
// `var c := t.Result;` and then stores two BORROWED parameters into `c`.
// The ownership pass runs on the generic template body, where the read's
// type is the bare parameter `T` (no class declaration), so it classifies
// `c` as a BORROW and admits both stores as borrow-to-borrow (§28.3 *State
// match on reassignment*, item 1: "a borrowed-state LHS requires a borrow (or
// `null`) RHS", v0.355). §28.7's drop-on-overwrite ("Assignment to an owned
// field or owned local disposes the displaced value") therefore does not
// apply to `c`: a borrowed-state store disposes nothing.
//
// The first fix-forward (9a8e479a) handed the shared fact codegen's
// monomorphization-aware resolver at the overwrite gate while the ownership
// pass handed it none, so at the `G<Cell>` specialization the gate resolved
// `T` to `Cell`, called `c` OWNED, and disposed the displaced value at EVERY
// store: the second store freed the caller's `b1`, and the caller's `b1.v`
// read was a heap-use-after-free (MEASURED on 9a8e479a under ASan). The gate
// now reads the fact in the ownership pass's own CLASSIFICATION form (no
// resolver), so the two sides cannot be handed different arguments.
//
// The oracle is must-not-UAF: there is no `heap-use-after-free` (nor any
// other AddressSanitizer ERROR headline, nor a signal) in the report, and
// the caller's reads of `b1.v` and `b2.v` after the call produced the exact
// sum (1 + 1 + 2 per iteration, 100 iterations). The row deliberately does
// NOT assert `r.ran`, the exit code or the live counter: the transferred
// `Result` of a GENERIC read is a borrow on both sides today and LEAKS (the
// base behaviour, a separate known gap; the ownership pass does not yet see
// the generic shape), and `runAotChecked` turns ANY sanitizer report — a
// leak-only one included — into `ran == false` / `exitCode == -1` with the
// report in the diagnostics, so pinning that leak is not this row's
// question. The vacuous-oracle controls are the exact stdout (a program
// ASan aborts at the first `b1.v` read prints no sum — MEASURED on
// 9a8e479a: empty stdout, `heap-use-after-free` in the report) and the
// toolchain-skip check.
BEZEL_TEST(owning_task_result_plain_var_8137,
           generic_body_reassign_from_borrowed_params_is_not_a_uaf_spec_28_7) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Mk(): OwningTask<Cell> return new Cell(5); end Mk; "
        "  static method G<T>(t: OwningTask<T>, b1: T, b2: T): int where T: class "
        "    var c := t.Result; "
        "    c := b1; "
        "    c := b2; "
        "    return 1; "
        "  end G; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t := Mk(); "
        "      var b1 := new Cell(1); var b2 := new Cell(2); "
        "      s := s + G<Cell>(t, b1, b2) + b1.v + b2.v; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("SUMMARY: AddressSanitizer: heap-use-after-free") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("program terminated by signal") ==
                      std::string::npos);
}

// §28.3 *State match on reassignment* -- the NON-GENERIC twin of the row
// above, the fourth fix-forward of #8137 (the R39 gate's ASan leg).
//
// In a non-generic body the ownership pass DOES see `var c := t.Result;` as
// §30.3's owned transfer, so `c` is an OWNED-state local, and `c := b1;`
// stores a BORROWED parameter into it. §28.3 (v0.355): "Assigning a borrow to
// an owned-state LHS, or an owned value to a borrowed-state LHS, is rejected
// -- the mismatch would silently drop or double-hold a disposal obligation",
// and item 1: "an owned-state LHS requires an owned RHS -- a fresh producer,
// or a named owned binding under the explicit transfer marker (`x := move
// y;`)". The program is therefore a COMPILE ERROR at the first store.
//
// MEASURED on r30-c1 @ c48c1c1c (ASan): the program compiled, the stores'
// drop-on-overwrite and `c`'s scope-end auto-drop disposed the CALLER's `b1`
// / `b2`, and the caller's `b1.v` read was a heap-use-after-free. (On
// origin/main d201a5c5, before #8137, `c` was a borrow: the program printed
// 400 and leaked the transferred Result.)
//
// The oracle: rejected at compile time with a real location (the program
// never ran), with the specific state-match diagnostic naming the borrow
// `b1` and the owned-state LHS `c`.
BEZEL_TEST(owning_task_result_plain_var_8137,
           nongeneric_reassign_from_borrowed_param_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Mk(): OwningTask<Cell> return new Cell(5); end Mk; "
        "  static method G(t: OwningTask<Cell>, b1: Cell, b2: Cell): int "
        "    var c := t.Result; "
        "    c := b1; "
        "    c := b2; "
        "    return 1; "
        "  end G; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t := Mk(); "
        "      var b1 := new Cell(1); var b2 := new Cell(2); "
        "      s := s + G(t, b1, b2) + b1.v + b2.v; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find(
        "cannot assign `b1` to `c`: `c` is in the OWNED state and the "
        "right-hand side is an unmarked read of `b1`, which is a BORROW") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("§28.3's state match on reassignment") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("heap-use-after-free") == std::string::npos);
}

// §28.3 *State match on reassignment*, item 1 -- an OWNED local read WITHOUT
// the transfer marker is the same violation: "a named owned binding under the
// explicit transfer marker (`x := move y;`)". `c := d;` over an owned `d`
// stores a borrow of `d`'s value into the owned-state `c`; MEASURED on
// r30-c1 @ c48c1c1c it compiled and both `c` and `d` disposed the same Cell.
// The diagnostic names the `move` remedy.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_result_reassign_from_unmarked_owned_local_is_rejected_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      var d := new Cell(8); "
        "      c := d; "
        "      s := s + c.v; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find(
        "cannot assign `d` to `c`: `c` is in the OWNED state and the "
        "right-hand side is an unmarked read of `d`, which is a BORROW") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("Spell the transfer with `move`") !=
                      std::string::npos);
}

// §28.3 / §28.7 -- the CONTROL for the row above: the marked transfer
// `c := move d;` is admitted (item 1's "named owned binding under the
// explicit transfer marker"), the DISPLACED transferred Result is disposed at
// the store (§28.7, b3940405's intent), and the moved Cell is disposed once at
// scope end through `c`: live counter 0, no leak.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_result_reassign_from_moved_owned_local_displaces_spec_28_7) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      s := s + c.v; "
        "      var d := new Cell(8); "
        "      c := move d; "
        "      s := s + c.v; "));
    expectNoLeak(r, "1300\nlive 0\n");
}

// =====================================================================
// THE UNWIND ROWS -- §28.3's EXCEPTION EDGE for a plain `var` bound to an
// `OwningTask`'s `Result` (the third fix-forward of #8137).
//
// `R` binds the transferred result to a plain `var`, THROWS on every other
// call (so an exception leaves the binding's scope on half the iterations),
// and otherwise MOVES the binding into an `owned` parameter. Because the
// binding is discharged on SOME path it is not auto-dropped at scope end
// (§28.3's affine auto-drop covers a never-discharged owner only), so the
// unwind-only disposal codegen registers for a plain owned local is its
// ONLY exception-edge disposal.
//
// §28.3 (v0.355): "every owned value is disposed exactly once on every
// path" -- the exception edge is a path. §30.3 (v0.355): the `Result` read
// "transfers ownership of the result to the reader exactly as `await`
// does", so the binding owns the value the unwind must dispose.
//
// PRE-FIX (the unit's tip, 45bac30d): the unit's §30.3 classification made
// the binding OWNED, so the ownership pass ACCEPTED the `move` -- the base
// (255cbe57) had REJECTED these programs with "a borrow cannot be passed
// where an `owned` value is required" -- but codegen's
// `registerUnwindOwnedLocal` keyed on `unwindOwnedLocalClass`, which
// classifies a local by its INITIALIZER FORM and has no arm for the `Result`
// read, so nothing was registered: MEASURED under ASan, 50 allocations /
// 800 bytes leaked and the class's live counter at 50 (the 50 throwing
// iterations), while the `new`-initialised twin was clean. The fix-forward
// reads the SAME shared classification fact the ownership pass reads at the
// registration, so the rows below go to `live 0` with no leak.
//
// The expected sum: 50 throwing iterations add 100 each (the catch arm) and
// 50 moving iterations add the moved object's value.
// =====================================================================

namespace {

// The program for the unwind rows. `rSig` is `R`'s first parameter (the
// task), `rBody` its body (which reads `k`, the throw switch), and `mk` the
// producer the caller binds with `using var t := ...`. `Sq` is a class
// behind the `IShape` interface for the interface-typed task.
std::string unwindProg(const std::string& rSig, const std::string& rBody,
                       const std::string& mk) {
    return
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "interface IShape "
        "  method V(): int; "
        "end IShape; "
        "class Sq: IShape "
        "  public static n: int := 0; "
        "  public constructor() Sq.n := Sq.n + 1; end constructor; "
        "  destructor Sq.n := Sq.n - 1; end destructor; "
        "  public method V(): int return 3; end V; "
        "end Sq; "
        "class C "
        "  public static async method Mk(): OwningTask<Cell> return new Cell(5); end Mk; "
        "  public static async method MkI(): OwningTask<IShape> return new Sq(); end MkI; "
        "  static method Boom(): int throw new InvalidOperationException(\"boom\"); end Boom; "
        "  static method TakeO(owned c: Cell): int return c.v; end TakeO; "
        "  static method TakeI(owned c: IShape): int return c.V(); end TakeI; "
        "  static method R(" + rSig + ", k: bool): int " + rBody + " end R; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t := " + mk + "; "
        "      try s := s + R(t, i mod 2 = 0); "
        "      catch var e: InvalidOperationException => s := s + 100; end try; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    Console.WriteLine($\"live {Cell.n} {Sq.n}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// §28.3 / §30.3 -- THE REVIEW'S REPRO (probe n16): `var c := t.Result;` then
// a throw on half the calls, else `TakeO(move c)`. Pre-fix: 50 allocations /
// 800 bytes leaked, live counter 50.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_is_disposed_on_the_unwind_edge_spec_28_3) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := t.Result; "
        "if k then var z := Boom(); end if; "
        "return TakeO(move c);",
        "Mk()"));
    expectNoLeak(r, "5250\nlive 0 0\n");
}

// §28.3 / §30.3 -- the ANNOTATED spelling (probe n16b): `var c: Cell :=
// t.Result;`. The declared type names the class; the obligation is the same.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_annotated_dot_result_is_disposed_on_the_unwind_edge_spec_28_3) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c: Cell := t.Result; "
        "if k then var z := Boom(); end if; "
        "return TakeO(move c);",
        "Mk()"));
    expectNoLeak(r, "5250\nlive 0 0\n");
}

// §28.3 / §30.3 -- the INTERFACE-typed result (probe n16i): `OwningTask<
// IShape>`, whose `Result` is an interface reference. The unwind disposal is
// dynamic (the object's own vtable), so the concrete `Sq` behind the
// interface is disposed; 50 moving iterations add `Sq.V()` = 3 each.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_interface_result_is_disposed_on_the_unwind_edge_spec_28_3) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<IShape>",
        "var c := t.Result; "
        "if k then var z := Boom(); end if; "
        "return TakeI(move c);",
        "MkI()"));
    expectNoLeak(r, "5150\nlive 0 0\n");
}

// §28.7 / §28.3 -- REASSIGN then throw (probe n16r): `c := new Cell(7);`
// disposes the transferred Cell by drop-on-overwrite (the second
// fix-forward), and the unwind edge must then dispose the NEW value -- the
// registration is on the SLOT, not on the transferred object. Pre-fix the
// new value leaked on the throw edge (50 allocations); 50 moving iterations
// add 7 each.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_reassigned_value_is_disposed_on_the_unwind_edge_spec_28_7) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := t.Result; "
        "c := new Cell(7); "
        "if k then var z := Boom(); end if; "
        "return TakeO(move c);",
        "Mk()"));
    expectNoLeak(r, "5350\nlive 0 0\n");
}

// §28.3 -- the CONTROL (probe n16c): the SAME shape on a `new`-initialised
// local, which `unwindOwnedLocalClass` has always recognised. It was clean
// before the fix and must stay clean -- the fix must not double-dispose it.
// (`R` ignores its task parameter.)
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_new_local_unwind_control_spec_28_3) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := new Cell(5); "
        "if k then var z := Boom(); end if; "
        "return TakeO(move c);",
        "Mk()"));
    expectNoLeak(r, "5250\nlive 0 0\n");
}

// ---------------------------------------------------------------------------
// THE DISCARD ROWS (third fix-forward, §17.13 / §28.3 / §30.3, v0.355).
//
// `_ := c;` on a plain local the §30.3 classification made OWNED is the
// announced consume of §17.13: the ownership pass stamps the discard
// (`discardOutOwnedVar`) and codegen's named-binding branch asks
// `explicitDisposeOwnedReceiverSlot` for the slot to reclaim + null. That
// resolver recognised a plain local only through `unwindOwnedLocalClass`
// (initializer FORM), so the Result-initialised local found no slot and the
// discard emitted nothing. Measured on 80e11e74 (the unwind fix-forward):
// 800 B in 50 allocations -- every NON-throwing iteration leaked, while the
// throwing half was rescued by the unwind entry. On its parent 480b4e18 the
// same program leaked all 100. The `new`-initialised twin was clean on both.
// Sums: 50 non-throwing iterations return 1, 50 throwing ones add 100.
// ---------------------------------------------------------------------------

// §17.13 / §28.3 -- DISCARD then throw (probe m04_r): the discard disposes
// the transferred Cell and nulls the slot; the throw edge then reloads null
// and skips, so the fix cannot double-dispose.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_discard_then_throw_disposes_exactly_once_spec_17_13) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := t.Result; "
        "_ := c; "
        "if k then var z := Boom(); end if; "
        "return 1;",
        "Mk()"));
    expectNoLeak(r, "5050\nlive 0 0\n");
}

// §17.13 / §28.3 -- throw then DISCARD (probe m08_r): the throwing half is
// disposed by the unwind entry, the non-throwing half by the discard.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_throw_then_discard_disposes_exactly_once_spec_17_13) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := t.Result; "
        "if k then var z := Boom(); end if; "
        "_ := c; "
        "return 1;",
        "Mk()"));
    expectNoLeak(r, "5050\nlive 0 0\n");
}

// §17.13 / §28.3 -- the CONTROL (probe m04_n): the same discard on a
// `new`-initialised local, clean before and after.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_new_local_discard_then_throw_control_spec_17_13) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := new Cell(5); "
        "_ := c; "
        "if k then var z := Boom(); end if; "
        "return 1;",
        "Mk()"));
    expectNoLeak(r, "5050\nlive 0 0\n");
}

// §28.9 / §28.3 -- explicit DISPOSE then throw (probe m03_r): the SAME
// resolver serves `c.Dispose()`, which the #4246 stamp arm already covered;
// the new structural arm answers first and must reclaim + null the same
// slot -- exactly once, with the unwind entry skipping the nulled slot.
BEZEL_TEST(owning_task_result_plain_var_8137,
           plain_var_dot_result_explicit_dispose_then_throw_disposes_exactly_once_spec_28_9) {
    JitResult r = runAotChecked(unwindProg(
        "t: OwningTask<Cell>",
        "var c := t.Result; "
        "c.Dispose(); "
        "if k then var z := Boom(); end if; "
        "return 1;",
        "Mk()"));
    expectNoLeak(r, "5050\nlive 0 0\n");
}

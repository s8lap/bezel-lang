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

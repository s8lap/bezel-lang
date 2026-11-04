// Regression tests for #8131 -- `using var c := o?.Result` on a nullable
// `OwningTask<Cell>` LEAKED the transferred result.
//
// THE DEFECT (MEASURED, on the tree that carries #7896/#7981's a4c902b8).
// `using var c := o?.Result;` -- `o` a nullable `OwningTask<Cell>` -- read the
// RIGHT value after #7896/#7981 taught the `?.` spelling to lower through the
// `.` form, but it LEAKED the transferred result: 16 bytes per read, 100
// allocations / 1600 bytes over a 100x loop under ASan/LSan. The `.` spelling
// (`using var c := t.Result;`) was clean.
//
// ROOT CAUSE. The `using var` scope-end disposal for this shape is registered
// by the classifier case (b0d) in `src/codegen/LlvmBackend_Setup.cpp`
// (`Emitter::usingVarOwnsClassObjectDynamically`), which recognised the
// initializer only in its MemberAccessExpr (`.`) spelling. The null-
// conditional spelling is a DIFFERENT AST node
// (`ast::NullCondMemberExpr`), so the classifier missed it: the ownership
// TRANSFER happened (the `?.` lowering routes the read through the same `.`
// lowering, so the task's result buffer is consumed) but no disposal was ever
// registered for the binding, and the transferred class object was freed by
// nobody.
//
// THE SPEC (language spec v0.355).
//
//   §30.3 (v0.355), `OwningTask<T>`: "reading Result on a COMPLETED OwningTask
//   in an owned-consuming position transfers ownership exactly like `await`".
//   The rule is about the read's POSITION, not its spelling.
//
//   §7.5 (v0.355): "`x?.M` returns null if `x` is null; otherwise it performs
//   the access. The result type is always nullable". So `o?.Result` performs
//   the very access `o.Result` performs -- §30.3's transfer included -- and
//   yields the NULLABLE of the member type.
//
//   §28.3 (v0.355): "`using var x := expr;` where `expr` is an owned producer
//   ... is owned, additionally subject to scope-based disposal at the end of
//   the enclosing block." A `using var` initializer is an owned-consuming
//   position, so the transferred value is owned by the binding and disposed at
//   scope end -- whatever spelling performed the read.
//
// THE ORACLE. Every leak row runs 100 iterations (a one-shot leak repro is a
// vacuous oracle) and asserts BOTH that the class's live counter is back to
// zero (the destructor ran) and that LeakSanitizer reports no leak. The
// `using var` rows run under `runAotChecked`, the ASan/LSan oracle, so the
// leak assertion is live in a NORMAL suite run and not only under the
// `BEZEL_AOT_FORCE_SANITIZE=address` sweep.
//
// THE CONTROL rows. The `.` spelling (the shape that was already clean), the
// NULL receiver (`o` is null: the `?.` short-circuits, nothing is transferred,
// and the disposal must be SKIPPED rather than firing on a null slot), a
// VALUE-typed `Task<int>?` receiver (no disposal obligation at all -- §28.9),
// and the plain `var c := o?.Result` spelling (no `using`), which this fix
// must NOT change and must NOT turn into a double free.
//
// RESIDUAL (deliberately NOT pinned as correct): the PLAIN `var` spelling
// leaks in BOTH spellings -- `var c := t.Result` (the `.` form) and
// `var c := o?.Result` (the `?.` form) each leak 16 bytes per read. That is a
// SPELLING-INDEPENDENT defect in the ownership pass's classification of the
// `.Result` read (it is not classified as an owned producer, so no auto-drop
// is flagged and no error is raised), NOT a consequence of this fix: the `.`
// spelling leaked identically before it. The plain-`var` row below therefore
// runs under `runAotCheckedLenient` (LeakSanitizer off, AddressSanitizer on)
// and pins what this fix must guarantee -- NO DOUBLE FREE -- while the
// residual leak is left for a separate fix.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// The program under test. `Cell` is a class with a destructor and a static
// live counter, so "the destructor ran" is directly observable; `Mk` is an
// `async` method returning `OwningTask<Cell>`, the §30.5 Pattern B producer.
// `body` is the loop body; the scaffolding loops 100 times and prints the
// accumulated sum and the final live count.
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

// §30.3 / §28.3 -- THE ISSUE'S REPRO, looped. A `using var` binding of the
// `?.` read of an `OwningTask<Cell>.Result` over a PRESENT receiver: the
// ownership transfer happens, so the binding owns the class object and
// disposes it at scope end. Pre-fix: 100 allocations / 1600 bytes leaked and
// the live counter stood at 100.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           using_var_nullcond_result_over_a_present_receiver_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      using var c := o?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §7.5 / §30.3 -- the NULL receiver. `o?.Result` short-circuits: nothing is
// read, nothing is transferred, and the binding holds null. The scope-end
// disposal must be SKIPPED (a null slot), not fired on a null pointer. This
// row is GREEN before the fix too -- it is the control that says the fix does
// not make the null path dispose anything.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           using_var_nullcond_result_over_a_null_receiver_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "      var on: OwningTask<Cell>? := null; "
        "      using var c := on?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "0\nlive 0\n");
}

// §30.3 -- the `.` spelling, the CONTROL. This shape was already clean
// (case (b0d) recognised it) and must stay clean: the fix must not disturb
// the `.` form's disposal, and in particular must not double-dispose.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           dot_form_control_is_unchanged_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      using var c := t.Result; "
        "      s := s + c.v; "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §30.3 -- the ANONYMOUS producer spelling: `Mk()?.Result`, a `?.` read whose
// receiver is a producer call rather than a named local. The `?.` lowering
// treats it as an unbound producer (the task frame is reclaimed at the read)
// and the transferred result is owned by the `using var` binding, so the
// binding disposes it at scope end. Pre-fix: 100 allocations / 1600 bytes
// leaked and the live counter stood at 100.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           using_var_nullcond_result_over_an_anonymous_producer_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var c := Mk()?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §30.3 / §28.3 -- the ANNOTATED `?.` spelling: `using var c: Cell? := o?.Result`.
// The declared type names the class, so the class-disposal path already
// reaches this binding; the row is the control that the fix changes nothing
// here (and, in particular, does not double-dispose a shape that was already
// discharged).
BEZEL_TEST(using_var_nullcond_task_result_8131,
           annotated_using_var_nullcond_result_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      using var c: Cell? := o?.Result; "
        "      s := s + (c?.v ?? 0); "));
    expectNoLeak(r, "500\nlive 0\n");
}

// §28.9 / §30.3 -- the VALUE-typed control: a `Task<int>?` receiver's
// `Result` is an `int`, which carries no disposal obligation, so no
// disposal may be registered for it (a registered one would dispose a
// non-heap value). The row pins the value (700 = 7 x 100) and the absence
// of any sanitizer finding.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           value_typed_task_result_carries_no_disposal_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class C "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t0 := Task.FromResult(7); "
        "      var t: Task<int>? := t0; "
        "      using var c := t?.Result; "
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

// §28.3 -- the PLAIN `var` spelling, the NO-DOUBLE-FREE control. This fix
// does not change this shape: the plain `var` leaks in BOTH spellings (the
// `.` form leaked identically before the fix -- a spelling-INDEPENDENT
// ownership-pass defect, reported separately), so this row runs with
// LeakSanitizer OFF (the residual leak would otherwise drown the row) and
// AddressSanitizer ON, and pins what this fix must guarantee: the `?.`
// spelling does not dispose the transferred value twice, and does not crash.
//
// The live counter stands at 100 -- the plain `var` binding never disposes
// the transferred value (neither spelling does). That number is the CURRENT
// behaviour of a shape this fix leaves alone, not an endorsement of it.
BEZEL_TEST(using_var_nullcond_task_result_8131,
           plain_var_nullcond_result_does_not_double_free_spec_28_3) {
    JitResult r = runAotCheckedLenient(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      var c := o?.Result; "
        "      s := s + (c?.v ?? 0); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\nlive 100\n");
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("attempting double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("heap-use-after-free") == std::string::npos);
}

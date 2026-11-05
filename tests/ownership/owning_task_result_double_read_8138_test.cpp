// Regression tests for #8138 -- reading `Result` TWICE from one
// `OwningTask` SEGFAULTED (both the `.` and the `?.` spelling).
//
// THE DEFECT (MEASURED, on the tree that carries #8131's dd45bbe2).
//   using var t := Mk();          // OwningTask<Cell>
//   using var c := t.Result;      // first read: consumes the result buffer
//   using var d := t.Result;      // second read: SIGSEGV (exit 139)
//
// ROOT CAUSE. §30.3's transfer rule CONSUMES the task's result buffer on the
// first read (the `bz_task_take_result` drain, so the task's own disposal
// does not double-free the transferred object), which leaves the buffer
// pointer NULL. The second read loaded the result THROUGH that null pointer --
// a plain null dereference, not a defined behaviour. The `await` sibling of
// the same rule already had its guard (`bz_task_is_spent` /
// `bz_task_mark_spent` at the await site, #1352); the `.Result` read had none,
// so the two spellings of the single-consumer rule disagreed.
//
// THE SPEC (language spec v0.355).
//
//   §30.3 (v0.355), `OwningTask<T>`: "reading it on a completed task in an
//   owned-consuming position transfers ownership of the result to the reader
//   exactly as `await` does -- the result buffer is consumed, and **a second
//   read or `await` raises `InvalidOperationException`**."
//
//   So the second read is a RUNTIME THROW of `InvalidOperationException` --
//   not a compile-time rejection, and not a null / default value. (The
//   language spec is the authority here: the STANDARD LIBRARY declares no
//   `OwningTask<T>` type and no `Result` property at all, so it is silent on
//   this question; §30.3 is explicit.)
//
//   §30.4 (v0.355) supplies the SPEND rule the throw implements: "the first
//   await spends the task regardless of outcome (success, fault, or
//   cancellation), and a second await raises `InvalidOperationException`".
//   The `.Result` read is that await's synchronous sibling, so it spends the
//   task on BOTH edges -- the success edge and the fault edge.
//
// THE ORACLE. Every row loops 100 times (a one-shot repro is a vacuous
// oracle) and runs under `runAotChecked`, the ASan/LSan oracle: a segfault
// or a leak turns the row RED.
//
// THE CONTROL rows. A SINGLE read still yields the value and raises nothing
// (the fix must not make the first read throw), a Pattern A `Task<T>` -- the
// MULTI-consumer family (§30.4: "A `Task` or `Task<T>` may be awaited from
// multiple places") -- can still be read twice without the throw, and a
// faulted `OwningTask` re-raises its ORIGINAL exception on the first read
// (only the SECOND read raises `InvalidOperationException`).

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

void expectCleanRun(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("detected memory leaks") == std::string::npos);
}

}  // namespace

// §30.3 -- THE ISSUE'S REPRO, looped, in the `.` spelling. The first read
// consumes the result buffer; the second read must raise
// `InvalidOperationException` (caught here) instead of dereferencing the
// drained buffer. Pre-fix: SIGSEGV (exit 139).
BEZEL_TEST(owning_task_result_double_read_8138,
           second_dot_result_read_raises_invalid_operation_exception_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      using var c := t.Result; "
        "      s := s + c.v; "
        "      try "
        "        _ := t.Result; "
        "        s := s + 1000; "
        "      catch var e: InvalidOperationException => "
        "        s := s + 10; "
        "      end; "));
    expectCleanRun(r, "1500\nlive 0\n");
}

// §30.3 / §7.5 -- the `?.` spelling: the same single-consumer rule, the same
// throw. §7.5 makes `o?.Result` perform the very access `o.Result` performs,
// so the read spends the task exactly as the `.` spelling does.
BEZEL_TEST(owning_task_result_double_read_8138,
           second_nullcond_result_read_raises_invalid_operation_exception_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      var o: OwningTask<Cell>? := t; "
        "      using var c := o?.Result; "
        "      s := s + (c?.v ?? 0); "
        "      try "
        "        _ := o?.Result; "
        "        s := s + 1000; "
        "      catch var e: InvalidOperationException => "
        "        s := s + 10; "
        "      end; "));
    expectCleanRun(r, "1500\nlive 0\n");
}

// §30.3 -- the CONTROL: a SINGLE read is not a spend. The value comes
// through and nothing is raised. (The fix must not make the FIRST read
// throw.)
BEZEL_TEST(owning_task_result_double_read_8138,
           single_result_read_is_not_a_spend_spec_30_3) {
    JitResult r = runAotChecked(prog(
        "      using var t := Mk(); "
        "      using var c := t.Result; "
        "      s := s + c.v; "));
    expectCleanRun(r, "500\nlive 0\n");
}

// §30.4 -- the PATTERN A CONTROL: a plain `Task<Cell>` is the MULTI-consumer
// family ("A `Task` or `Task<T>` may be awaited from multiple places"), so
// reading its `Result` twice is NOT a single-consumer violation and must not
// raise. The two reads yield the same borrowed object, and the TASK owns it
// (the task's disposal disposes it -- §30.5 Pattern A), so the live counter
// returns to 0.
BEZEL_TEST(owning_task_result_double_read_8138,
           pattern_a_task_result_can_be_read_twice_spec_30_4) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Mk(): Task<Cell> return new Cell(5); end Mk; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t := Mk(); "
        "      var c := t.Result; "
        "      var d := t.Result; "
        "      s := s + c.v + d.v; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    Console.WriteLine($\"live {Cell.n}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectCleanRun(r, "1000\nlive 0\n");
}

// §30.4 -- the FAULT-EDGE spend: "the first await spends the task regardless
// of outcome (success, fault, or cancellation)". The first read of a FAULTED
// task re-raises the ORIGINAL exception (§30.3 / §30.9) and spends the task;
// the SECOND read raises `InvalidOperationException` (not the original
// exception again). The two catches are distinguishable by type: the first
// catches `MyErr`, the second catches `InvalidOperationException`.
BEZEL_TEST(owning_task_result_double_read_8138,
           faulted_task_first_read_raises_original_then_second_raises_ioe_spec_30_4) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Cell "
        "  public static n: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class MyErr: Exception "
        "  public constructor() end constructor; "
        "end MyErr; "
        "class C "
        "  public static async method MkBad(): OwningTask<Cell> "
        "    throw new MyErr(); "
        "  end MkBad; "
        "  static method A() "
        "    var i := 0; var s := 0; "
        "    while i < 100 do "
        "      using var t := MkBad(); "
        "      try "
        "        _ := t.Result; "
        "        s := s + 1000; "
        "      catch var e: MyErr => "
        "        s := s + 1; "
        "      end; "
        "      try "
        "        _ := t.Result; "
        "        s := s + 1000; "
        "      catch var e: InvalidOperationException => "
        "        s := s + 10; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(s); "
        "  end A; "
        "  public static method Main(): int "
        "    A(); "
        "    Console.WriteLine($\"live {Cell.n}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectCleanRun(r, "1100\nlive 0\n");
}

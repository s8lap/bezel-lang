// Spec-derived regression test for #7322 —
// `await using var` over a right-hand side that carries NO disposal
// obligation read 8 bytes out of bounds (a heap-buffer-overflow), because
// the `await using` scope-end path applied a CLASS disposal to a slot that
// does not hold a class object.
//
// AUTHORITY.
//
//   spec §28.12 (v0.344), Bezel_Language_Spec.md:7488 — "`await using var`
//   takes §28.10's admission unchanged — the adoption of an owned producer or
//   a marked transfer, the rejection of an unmarked named owned binding, the
//   value-type, runtime-managed and move-only right-hand sides, and *`using
//   var` initialized from a borrow*. Over a borrow right-hand side (a call
//   without an `owned` return, a field, an indexer, a borrow-state binding)
//   the binding assumes no obligation, so scope exit disposes nothing and
//   awaits nothing: the form is the same compile-time pin as under `using
//   var`, with the same freeze on the source. Over an owned right-hand side
//   with no `DisposeAsync` to await — a value-type, runtime-managed or
//   move-only right-hand side, or an object whose class does not implement
//   `IAsyncDisposable` — scope exit takes the synchronous disposal (or the
//   no-op) and likewise awaits nothing."
//
//   spec §28.12 (v0.344), Bezel_Language_Spec.md:7490 — "`await using`
//   selects the logical-disposal path by the class of the object the binding
//   holds — as the synchronous path selects the destructor chain it runs —
//   never by the binding's static type. ... Only an object whose class does
//   not implement `IAsyncDisposable` takes the synchronous disposal above."
//   A VALUE type and a RUNTIME-MANAGED value have no class at all, so their
//   scope exit is the no-op.
//
//   spec §28.10 (v0.344), Bezel_Language_Spec.md:7260 — "`using var` accepts
//   a value-type or runtime-managed right-hand side. For ordinary value types
//   and runtime-managed types, the disposal at scope end is a no-op (these
//   types do not require disposal per §28.9), and the only effect is the
//   reassignability restriction."
//
//   spec §28.9 (v0.344), Bezel_Language_Spec.md:7142-7147 — `string` is
//   runtime-managed; its `Dispose()` is a no-op.
//
// THE DEFECT. `registerUsingDisposal`'s `ld->isAwaitUsing` branch registered
// the async/class disposal (`Disposal{asyncDispose}`) for EVERY right-hand
// side that was not a builtin collection, a TaskScope, or an async
// enumerator. `emitDisposals` routes that to `emitAsyncDisposeAwaitAndFree`,
// whose null-`disposeAsyncFn` fallback is `emitDisposeAndReclaim` ->
// `emitDynamicDispose` -> *load word 0, call vtable slot 2*. That fallback
// is sound only when word 0 IS a class vtable. It is not, for these shapes:
//
//   `await using var t := o.ToString();`   word 0 of a bz.string is its DATA
//                                           pointer, so slot 2 is read at
//                                           data+16 — 14 bytes past the end
//                                           of the 2-byte payload. ASan:
//                                           heap-buffer-overflow, READ of
//                                           size 8.
//   `await using var x := 5;`               the slot holds an i64; the load
//                                           reads past it. ASan:
//                                           stack-buffer-overflow.
//   `await using var x := "hi";`            slot 2 is a null pointer. ASan:
//                                           SEGV on unknown address 0x0.
//   `await using var x := 1.5;`             a wild jump. ASan: SEGV.
//   `await using var p := new Point(1, 2);` a record's first word is read as
//                                           a vtable. ASan: SEGV.
//
// The plain `using var` spelling over EVERY one of those right-hand sides
// was already correct (it prints the value and exits 0, ASan+LSan clean) —
// so the `await` keyword alone was the difference.
//
// THE ORACLE. Every row runs the program through `runAotChecked`, the AOT
// oracle with the generated code instrumented by AddressSanitizer and
// LeakSanitizer. A row asserts FOUR things: that no sanitizer report and no
// crash occurred (this is the OVER-READ DETECTOR — on the pre-fix compiler
// these rows see "AddressSanitizer: heap-buffer-overflow ... READ of size 8",
// or a SIGSEGV, reported by the harness), that the program ran, that its
// exit status is 0, and that its stdout is EXACTLY the expected text. A row
// that asserted only "exit code 0" would also pass on a compiler that died
// before emitting anything.
//
// The controls — the plain `using var` spelling, the sanctioned
// `await using var u := new Solo();` over a real `IAsyncDisposable`, and the
// interface-typed right-hand side — are GREEN ON BOTH ARMS: they are what
// proves the fix did not widen any other behaviour.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The shared oracle. `expectedStdout` is the program's EXACT stdout.
//
// The three "no sanitizer report" assertions are the over-read detector: the
// harness embeds the sanitizer's report text in the diagnostic it reports
// when the report fired, so these rows FAIL on the pre-fix compiler with the
// report in the failure message — they do not merely observe "the program
// did not crash".
void expectRunsClean(const std::string& program,
                     const std::string& expectedStdout) {
    JitResult r = runAotChecked(program);
    // A missing C toolchain is a SKIP, and it must be LOUD rather than
    // absorbed (#3733): without the ASan leg this row proves nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "AddressSanitizer"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "heap-buffer-overflow"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// (The memory-safety-only oracle that once stood here — "the compiler emitted
// no out-of-bounds access", saying nothing about whether the program compiled
// and ran — is GONE, and it is gone because it is what made a row VACUOUS.
// Its only user was the row whose program was the corpus case s28_12_0015's,
// in a PLAIN (non-async) `Main`; §28.12 now REJECTS that program, so "no
// out-of-bounds access" was satisfied by a compile-time rejection and the row
// could no longer fail. The row was RE-SPELLED into an admitted context (an
// `async` lambda) and now uses the FULL oracle above, which asserts the
// ADMISSION — the program ran, exited 0, printed — as well as the absence of
// the over-read.)

// ---- Source shapes -------------------------------------------------------

// The SHAPE this defect is about: a runtime-managed right-hand side (a
// `string` returned by a call — a borrow right-hand side per §28.10's "using
// var initialized from a borrow"), inside an async method, which is the
// context §28.12 admits.
const char* kVacuousStringAsync =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var o := 1; "
    "    await using var t := o.ToString(); "
    "    Console.WriteLine($\"[{t}]\"); "
    "  end Main; "
    "end Program;";

// The same binding with an INT right-hand side (a value type): the slot holds
// an i64 and the disposal read a pointer out of it.
const char* kVacuousIntAsync =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var x := 5; "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// A STRING LITERAL right-hand side: the slot held a sentinel and vtable slot
// 2 was a null pointer.
const char* kVacuousStringLiteralAsync =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var x := \"hi\"; "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// A DOUBLE right-hand side (a value type).
const char* kVacuousDoubleAsync =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var x := 1.5; "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// A RECORD right-hand side (a value type, §9): the record's first word was
// read as a class vtable.
const char* kVacuousRecordAsync =
    "module Conf; "
    "record Point(X: int, Y: int); "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var p := new Point(1, 2); "
    "    Console.WriteLine($\"[{p.X}]\"); "
    "  end Main; "
    "end Program;";

// The CONTROL: the plain `using var` spelling over the SAME right-hand side.
// Green on both arms — §28.10's spelling, unchanged by this fix.
const char* kPlainUsingVarStringAsync =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var o := 1; "
    "    using var t := o.ToString(); "
    "    Console.WriteLine($\"[{t}]\"); "
    "  end Main; "
    "end Program;";

// The SANCTIONED shape: a real `IAsyncDisposable`. §28.12 step 1 — the
// awaited `DisposeAsync` performs the asynchronous cleanup and the
// destructor body is NOT also run. Green on both arms.
const char* kSanctionedAsyncDisposable =
    "module Conf; "
    "public class Solo: IAsyncDisposable "
    "  public constructor() "
    "  end constructor; "
    "  destructor "
    "    Console.WriteLine(\"sync released\"); "
    "  end destructor; "
    "  public async method DisposeAsync(): Task "
    "    Console.WriteLine(\"async released\"); "
    "  end DisposeAsync; "
    "end Solo; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var u := new Solo(); "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// The INTERFACE-typed right-hand side: §28.12:7490 says the disposal path is
// chosen "by the class of the object the binding holds", so an
// interface-typed binding must still dispose the RUNTIME class's instance.
// Both the destructor and DisposeAsync print the same marker, so this row
// pins "the object was disposed EXACTLY ONCE" without pinning WHICH of the
// two §28.12 paths ran — that choice is a separate, still-open question (the
// pre-fix compiler takes the synchronous path here, and this fix does not
// change that). The row's subject is the regression risk of this fix: an
// interface-typed binding must not be classified as value-like and lose its
// disposal entirely. Green on both arms.
const char* kInterfaceTypedAsyncDisposable =
    "module Conf; "
    "public class Solo: IAsyncDisposable "
    "  public constructor() "
    "  end constructor; "
    "  destructor "
    "    Console.WriteLine(\"disposed\"); "
    "  end destructor; "
    "  public async method DisposeAsync(): Task "
    "    Console.WriteLine(\"disposed\"); "
    "  end DisposeAsync; "
    "end Solo; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var d: IAsyncDisposable := new Solo(); "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// The corpus case s28_12_0015's PROGRAM, RE-SPELLED into a context §28.12
// ADMITS: the same vacuous binding (a runtime-managed right-hand side — a
// `string` returned by a call, a borrow right-hand side per §28.10's "using
// var initialized from a borrow") inside an `async` LAMBDA, which §28.12
// names beside the async method ("`await using` is valid only inside an
// async method or async lambda" — §28.12, v0.345, spec:7502).
//
// ⚠ WHY IT IS RE-SPELLED, AND WHY THE OLD SHAPE COULD NOT FAIL. The row
// used to run the program in a PLAIN (non-async) `Main` and asserted MEMORY
// SAFETY ONLY ("the compiler emitted no out-of-bounds access"), deliberately
// saying nothing about the program's acceptance, so that the row would stay
// true whether or not the missing §28.12 rejection landed. That rejection has
// landed (fix #7326: `await using` outside an async context is a compile
// error at the declaration — §28.12, v0.345, spec:7502), so the program is
// now REJECTED at compile time — and a rejected program emits no
// out-of-bounds access, so the row's only assertion was satisfied VACUOUSLY
// and the row COULD NO LONGER FAIL. The row is now pinned in an ADMITTED
// context and uses the FULL oracle: the program must be ADMITTED (it ran,
// exited 0, printed "[1]") AND free of the over-read. The enclosing `Main`
// is still NOT async — the binding lives in the `async` lambda's own frame,
// which is the shape that differs from the method-shaped row above.
const char* kVacuousStringAsyncLambda =
    "module Conf; "
    "public class Program "
    "  public static method Main() "
    "    var f := async lambda() => do "
    "      var o := 1; "
    "      await using var t := o.ToString(); "
    "      Console.WriteLine($\"[{t}]\"); "
    "    end; "
    "    using var task := f(); "
    "    task.Wait(); "
    "  end Main; "
    "end Program;";

// The vacuous binding in an async method that ALSO has a genuine suspension
// point after it — the state machine's own resume path, with the binding
// living in the frame across the suspend.
const char* kVacuousStringAsyncAcrossSuspend =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var o := 1; "
    "    await using var t := o.ToString(); "
    "    await Task.Delay(1); "
    "    Console.WriteLine($\"[{t}]\"); "
    "  end Main; "
    "end Program;";

}  // namespace

// ===========================================================================
// §28.12 / §28.9 / §28.10 — the vacuous `await using` right-hand sides.
// ===========================================================================

// §28.12:7488 — a runtime-managed (borrow) right-hand side assumes NO
// obligation, so scope exit disposes nothing and awaits nothing. The
// pre-fix compiler instead ran a class disposal over the string and read
// 8 bytes at payload+16 (heap-buffer-overflow).
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_over_a_runtime_managed_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousStringAsync, "[1]\n");
}

// §28.12:7488 / §28.9 — a value-typed (int) right-hand side assumes no
// obligation either; the pre-fix compiler read a pointer out of the i64
// slot (stack-buffer-overflow).
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_over_an_int_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousIntAsync, "[5]\n");
}

// §28.12:7488 — a string LITERAL right-hand side; the pre-fix compiler
// called a null vtable slot (SEGV at 0x0).
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_over_a_string_literal_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousStringLiteralAsync, "[hi]\n");
}

// §28.12:7488 — a value-typed (double) right-hand side; the pre-fix
// compiler jumped through a garbage "vtable" (SEGV).
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_over_a_double_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousDoubleAsync, "[1.5]\n");
}

// §28.12:7488 — a RECORD right-hand side (§9, a value type); the pre-fix
// compiler read the record's first word as a class vtable (SEGV).
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_over_a_record_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousRecordAsync, "[1]\n");
}

// §28.12:7488 — the same binding across a genuine suspension point: the
// state machine's own resume path.
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_across_a_suspend_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kVacuousStringAsyncAcrossSuspend, "[1]\n");
}

// §28.12 (v0.345, spec:7502) / the corpus case s28_12_0015's program,
// RE-SPELLED into an `async` lambda — the context §28.12 admits — so that
// the row asserts BOTH that the program is ADMITTED and that the over-read
// does not happen. See the shape's comment for why the plain-`Main` spelling
// had to go.
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_vacuous_await_using_in_an_async_lambda_in_a_plain_main_reads_no_out_of_bounds_spec_28_12) {
    expectRunsClean(kVacuousStringAsyncLambda, "[1]\n");
}

// ===========================================================================
// CONTROLS — green on BOTH arms; they prove the fix widened nothing.
// ===========================================================================

// §28.10:7260 — the plain `using var` spelling over the SAME right-hand
// side is unchanged: it disposes nothing at scope end and prints the value.
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_plain_using_var_spelling_over_the_same_rhs_is_unchanged_spec_28_10) {
    expectRunsClean(kPlainUsingVarStringAsync, "[1]\n");
}

// §28.12 step 1 — the SANCTIONED shape: a real `IAsyncDisposable`. The
// awaited `DisposeAsync` performs the cleanup and the destructor body is
// NOT also run.
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_await_using_over_an_i_async_disposable_awaits_its_dispose_async_spec_28_12) {
    expectRunsClean(kSanctionedAsyncDisposable, "body\nasync released\n");
}

// §28.12:7490 — an INTERFACE-typed right-hand side still disposes the
// runtime class's instance, exactly once.
BEZEL_TEST(await_using_vacuous_rhs_7322,
           the_await_using_over_an_interface_typed_rhs_disposes_the_runtime_class_spec_28_12) {
    expectRunsClean(kInterfaceTypedAsyncDisposable, "body\ndisposed\n");
}

// Spec-derived regression test for #7337 and #7327 —
// the `await using` slot predicate decided from the binding's DECLARED type
// alone, so a DECLARED `Object` (or a declared function type) binding over a
// right-hand side that is NOT a class still ran the class disposal and read
// out of bounds.
//
// AUTHORITY.
//
//   spec §28.12 (v0.344), Bezel_Language_Spec.md:7490 — "**The `DisposeAsync`
//   awaited is the run-time object's.** `await using` selects the
//   logical-disposal path by the class of the object the binding holds — as
//   the synchronous path selects the destructor chain it runs — never by the
//   binding's static type. ... through `Object`, and through any interface
//   ... Only an object whose class does not implement `IAsyncDisposable`
//   takes the synchronous disposal above. The static type decides nothing
//   about which cleanup runs; it decides only what is known before run
//   time." => THE STATIC TYPE IS NOT THE AXIS; THE OBJECT THE BINDING HOLDS
//   IS. What the compiler can know about that object is the RIGHT-HAND SIDE'S
//   own type.
//
//   spec §28.12 (v0.344), Bezel_Language_Spec.md:7488 — "`await using var`
//   takes §28.10's admission unchanged — ... the value-type, runtime-managed
//   and move-only right-hand sides ... Over an owned right-hand side with no
//   `DisposeAsync` to await — a value-type, runtime-managed or move-only
//   right-hand side, or an object whose class does not implement
//   `IAsyncDisposable` — scope exit takes the synchronous disposal (or the
//   no-op) and likewise awaits nothing."
//
//   spec §28.9 (v0.344), Bezel_Language_Spec.md:7147 — "**Function-typed
//   values** — values of any function type `(T1, ..., Tn) -> R`, regardless
//   of whether they capture. ... Function-typed fields, parameters, and
//   locals are not subject to `owned` annotations and carry no disposal
//   obligation; a `using var` local of function type is nonetheless
//   permitted (disposal at scope end is a no-op) and serves only the
//   non-reassignability role (§28.10)."
//
//   spec §28.9 (v0.344), Bezel_Language_Spec.md:7142 — `string` is the first
//   of the runtime-managed types: exempt from manual disposal, its
//   `Dispose()` is a no-op.
//
//   spec §28.10 (v0.344), Bezel_Language_Spec.md:7260 — "`using var` accepts
//   a value-type or runtime-managed right-hand side. For ordinary value types
//   and runtime-managed types, the disposal at scope end is a no-op (these
//   types do not require disposal per §28.9), and the only effect is the
//   reassignability restriction."
//
// THE DEFECT. #7322 introduced the predicate that decides whether the
// `await using` slot may hold a class at all. It read the DECLARED type
// first and the initializer's own type only when there was no declared
// type, so:
//
//   `await using var x: Object := o.ToString();`  the declared type says
//       "may hold a class", the slot holds a `bz.string` whose word 0 is
//       its DATA pointer, so vtable slot 2 is read at data+16 — 14 bytes
//       past the end of the 2-byte payload. ASan: heap-buffer-overflow,
//       READ of size 8.
//   `await using var x: Object := "hi";`           the literal's slot 2 is a
//       null pointer. ASan: SEGV on unknown address 0x0.
//   `await using var f := lambda() => 1;`          `sema::TypeRef::
//       Kind::Function` was never named by the predicate's switch, so it fell
//       through to the conservative "may hold a class" answer and the
//       closure's word 0 was read as a vtable. ASan: SEGV.
//   `await using var f: () -> int := lambda() => 1;`  the same, with the
//       function type DECLARED. ASan: SEGV.
//
// The plain `using var` spelling over every one of those right-hand sides
// was already correct, so the twins disagreed.
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
// THE MARKERS ARE DISTINCT, and that is the point: the destructor prints
// "SYNC" and `DisposeAsync` prints "ASYNC", so a row can TELL which of the
// two §28.12 paths ran. (The rows in
// `await_using_vacuous_rhs_7322_test.cpp` print the SAME marker from both,
// so their interface-typed control row cannot tell the paths apart — a
// vacuous oracle. These rows do not repeat that mistake.)
//
// The controls — the plain `using var` spelling over the same right-hand
// sides, and the `Object`-declared class right-hand sides — are GREEN ON
// BOTH ARMS: they are what proves the fix did not widen any other behaviour.
// In particular the `Object`-declared CLASS right-hand side must keep its
// asynchronous disposal (§28.12:7490's "through `Object`" case).

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

// ---- Source shapes -------------------------------------------------------

// The #7337 shape: the binding is DECLARED `Object` and holds a
// runtime-managed value (a `string` returned by a call). The declared type
// says "may hold a class"; the value held has no class at all.
const char* kObjectDeclaredStringRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var o := 1; "
    "    await using var x: Object := o.ToString(); "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// The same shape with a string LITERAL right-hand side: the literal's slot 2
// is a null pointer.
const char* kObjectDeclaredStringLiteralRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var x: Object := \"hi\"; "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// The #7327 shape: a FUNCTION-typed right-hand side, with no declared type
// (so the predicate never named `sema::TypeRef::Kind::Function` at all).
const char* kFunctionTypedRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var f := lambda() => 1; "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// The same shape with the function type DECLARED: the DECLARED type is the
// function type itself, so the predicate's switch had to name
// `Kind::Function` to answer at all.
const char* kDeclaredFunctionTypedRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var f: () -> int := lambda() => 1; "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// THE MIRROR: the `Object`-declared binding holds a CLASS, so §28.12:7490's
// "through `Object`" case applies and the ASYNCHRONOUS path must run — the
// awaited DisposeAsync, printing "ASYNC" and NOT the destructor's "SYNC".
// This row is the regression risk of the fix: the narrowing must not cost a
// class its asynchronous disposal. Green on both arms.
const char* kObjectDeclaredClassRhs =
    "module Conf; "
    "public class Solo: IAsyncDisposable "
    "  public constructor() "
    "  end constructor; "
    "  destructor "
    "    Console.WriteLine(\"SYNC\"); "
    "  end destructor; "
    "  public async method DisposeAsync(): Task "
    "    Console.WriteLine(\"ASYNC\"); "
    "  end DisposeAsync; "
    "end Solo; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var u: Object := new Solo(); "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// The `Object`-declared binding holds a class that does NOT implement
// `IAsyncDisposable`: §28.12:7488's "an object whose class does not
// implement `IAsyncDisposable` — scope exit takes the synchronous disposal".
// The distinct markers pin that the SYNCHRONOUS path ran ("SYNC"), not the
// asynchronous one. Green on both arms.
const char* kObjectDeclaredClassWithoutAsyncRhs =
    "module Conf; "
    "public class Plain "
    "  public constructor() "
    "  end constructor; "
    "  destructor "
    "    Console.WriteLine(\"SYNC\"); "
    "  end destructor; "
    "end Plain; "
    "public class Program "
    "  public static async method Main(): Task "
    "    await using var u: Object := new Plain(); "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

// CONTROL: the plain `using var` spelling over the SAME right-hand side as
// the first row — unchanged by this fix (§28.10:7260). Green on both arms.
const char* kPlainUsingVarObjectDeclaredStringRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var o := 1; "
    "    using var x: Object := o.ToString(); "
    "    Console.WriteLine($\"[{x}]\"); "
    "  end Main; "
    "end Program;";

// CONTROL: the plain `using var` spelling over the SAME right-hand side as
// the function-typed row — unchanged by this fix. Green on both arms.
const char* kPlainUsingVarFunctionTypedRhs =
    "module Conf; "
    "public class Program "
    "  public static async method Main(): Task "
    "    using var f := lambda() => 1; "
    "    Console.WriteLine(\"body\"); "
    "  end Main; "
    "end Program;";

}  // namespace

// ===========================================================================
// §28.12 / §28.9 / §28.10 — the DECLARED-type `await using` right-hand
// sides the predicate misread.
// ===========================================================================

// §28.12:7490 — the disposal path is selected by the class of the object
// the binding HOLDS, never by the binding's static type. A DECLARED `Object`
// binding over a runtime-managed value holds no class, so its scope exit is
// the no-op. The pre-fix compiler ran the class disposal over the string and
// read 8 bytes at payload+16 (heap-buffer-overflow).
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_an_object_declared_string_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kObjectDeclaredStringRhs, "[1]\n");
}

// §28.12:7490 / §28.9 — the same shape with a string LITERAL; the pre-fix
// compiler called a null vtable slot (SEGV at 0x0).
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_an_object_declared_string_literal_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kObjectDeclaredStringLiteralRhs, "[hi]\n");
}

// §28.9 — a FUNCTION-typed value is runtime-managed: it carries no disposal
// obligation and a `using var` local of function type is permitted with a
// no-op disposal. The pre-fix compiler never named `Kind::Function` and read
// the closure's word 0 as a vtable (SEGV).
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_a_function_typed_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kFunctionTypedRhs, "body\n");
}

// §28.9 — the same, with the function type DECLARED: the declared type is
// the function type, so the predicate's switch had to name `Kind::Function`
// to answer at all. The pre-fix compiler read the closure's word 0 as a
// vtable (SEGV).
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_a_declared_function_typed_rhs_is_not_an_async_disposal_spec_28_12) {
    expectRunsClean(kDeclaredFunctionTypedRhs, "body\n");
}

// ===========================================================================
// CONTROLS — green on BOTH arms; they prove the fix widened nothing.
// ===========================================================================

// §28.12:7490 — the `Object`-declared binding over a CLASS still takes the
// ASYNCHRONOUS path: "ASYNC" (the awaited DisposeAsync) and NOT "SYNC" (the
// destructor). This is §28.12:7490's "through `Object`" case, and it is the
// row that would catch a narrowing which cost a class its async disposal.
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_an_object_declared_class_rhs_awaits_its_dispose_async_spec_28_12) {
    expectRunsClean(kObjectDeclaredClassRhs, "body\nASYNC\n");
}

// §28.12:7488 — an `Object`-declared binding over a class that does NOT
// implement `IAsyncDisposable` takes the SYNCHRONOUS disposal: "SYNC". The
// distinct markers tell the two §28.12 paths apart.
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_await_using_over_an_object_declared_class_rhs_without_dispose_async_takes_the_sync_path_spec_28_12) {
    expectRunsClean(kObjectDeclaredClassWithoutAsyncRhs, "body\nSYNC\n");
}

// §28.10:7260 — the plain `using var` spelling over the SAME right-hand
// side is unchanged: it disposes nothing at scope end and prints the value.
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_plain_using_var_spelling_over_the_same_object_declared_rhs_is_unchanged_spec_28_10) {
    expectRunsClean(kPlainUsingVarObjectDeclaredStringRhs, "[1]\n");
}

// §28.10:7260 / §28.9 — the plain `using var` spelling over a
// function-typed right-hand side is unchanged.
BEZEL_TEST(await_using_declared_rhs_predicate_7337,
           the_plain_using_var_spelling_over_a_function_typed_rhs_is_unchanged_spec_28_10) {
    expectRunsClean(kPlainUsingVarFunctionTypedRhs, "body\n");
}

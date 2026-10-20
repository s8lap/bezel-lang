// Codegen / leak — a `string` LOCAL bound to a +1 producer and then passed to
// Console.Write/WriteLine (or the Console.Out/Console.Error facets) leaked its
// single reference. Issue #1994.
//
// Spec: §4.6 (`string` is runtime-managed — its memory is reclaimed
// automatically), §28.9 (runtime-managed types; the string refcount discipline),
// §28.3 (local-variable ownership and borrows), §2.1 (`Console`) / §7.4
// (`TextWriter` — the Console.Out/Console.Error facets). Spec revision v0.197.
//
// THE BUG. A `var` string local bound to a +1 producer (a user-method string
// return, a numeric `.ToString()`, a `+` concat, an interpolation, …) owns that
// single reference and must drop it at scope end — UNLESS it escapes. The
// scope-end release is gated on the string-escape walk (StringEscapeWalker),
// which classifies a bare read of the local in a CALL-ARGUMENT position as an
// ESCAPE unless the callee is a resolved user/imported plain-borrow method (the
// #1430 relaxation) or the retaining StringBuilder.Append (the #1185 relaxation).
// `Console.Write`/`Console.WriteLine` is a §2.3 INTRINSIC (special-lowered
// structurally by name in tryLowerConsoleCall) — it has no resolved MethodSymbol,
// so the #1430 path never fires and the argument stayed ESCAPED. An escaping local
// is excluded from the scope-end release, so the producer's +1 was never dropped:
// it leaked (32-byte header + data buffer), once per such local. But WriteLine
// BORROWS its argument — emitConsoleWrite reads the bytes into write(2) and
// returns, holding no onward reference (the #1031 direct-producer path proves this:
// it RELEASES a fresh +1 argument right after the write). So the escape is a false
// positive and the leak is real.
//
// Distinct from the #429 argument-pass leak family: a resolved user plain-borrow
// method already keeps the local released (#1430 verified — `C.Borrow(s)` is
// leak-clean on base). The gap is specific to the non-retaining string-consuming
// INTRINSICS that carry no callee-side borrow discipline.
//
// THE FIX (LlvmBackend_Setup.cpp, StringEscapeWalker::isConsoleNonRetainingStringArg
// + walkCallArgs): a string local passed in the value position (index 0) of
// Console.Write/WriteLine — or the Console.Out/Console.Error facets that route
// through the same lowering — is a CALL-BOUNDED borrow, not an escape, so the local
// stays scope-end-release eligible. Recognized structurally by the SAME callee
// shapes tryLowerConsoleCall keys on, so the escape decision and the lowering stay
// in lockstep. Gated on stringReleaseMode (the #1997 narrowing) so the shared
// dict/grouping/borrowing-Array walks keep their conservative ESCAPE.
//
// NO owned-sink hazard. `owned` is inadmissible on a `string` parameter (§28.1 —
// strings are runtime-managed reference types with no disposal obligation to
// transfer), so there is no string owned-sink that could double-free. The only
// "adopt" for a string is a field store (which mints its own +1, #429) or an
// escape position (return/new/assign), both independently tainting the local — the
// return-transfer guards below pin that WriteLine's relaxation does not override
// them.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan; a
// leaked string OR a double-free/UAF (over-releasing a value the call still borrows,
// or one that escapes) flips r.ran to false. The exit code pins the value via
// `.Length`, so a premature free would corrupt it as well as flip r.ran.
//
// Two kinds of case:
//   * LEAK REPROS (A,B,C,D,E,H) — LSan-leak on base 35e854b, LSan-clean with the
//     fix (fail-without / pass-with).
//   * DOUBLE-FREE / TRANSFER GUARDS (F,G) — LSan-clean BOTH before and after the
//     fix: a local WriteLine'd AND returned (the return transfers the +1, which
//     must still exclude the local from the scope-end release), and literal /
//     interpolation direct arguments (no local — the #1031 direct-producer path
//     already releases them). These prove the fix introduces no double-free and
//     leaves the direct-producer path untouched.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// =====================================================================
// LEAK REPROS — LSan-leak on base 35e854b, LSan-clean with the fix.
// =====================================================================

// (A) The #1994 repro: a USER-METHOD string-return producer bound to a local,
// then WriteLine'd. The producer's +1 (36 bytes) leaked on base.
const char* kUserMethodReturnLocal =
    "module M; class C "
    "  public static method Render(): string "
    "    var n := 7; "
    "    return \"n=\" + n.ToString(); "
    "  end Render; "
    "  public static method Main(): int "
    "    var s := C.Render(); "
    "    Console.WriteLine(s); "          // borrow — must not exclude s from release
    "    return s.Length; "               // "n=7".Length = 3
    "  end Main; "
    "end C;";

// (B) A `+`-concat producer local written with Console.Write (no newline). Same
// escape false-positive on the non-WriteLine overload.
const char* kConcatLocalWrite =
    "module M; class C "
    "  public static method Main(): int "
    "    var n := 4; "
    "    var s := \"v=\" + n.ToString(); "
    "    Console.Write(s); "
    "    return s.Length; "               // "v=4".Length = 3
    "  end Main; "
    "end C;";

// (C) Console.Error.WriteLine facet — routes through the same tryLowerConsoleCall
// stderr path; leaked identically on base.
const char* kConsoleErrorFacet =
    "module M; class C "
    "  public static method Main(): int "
    "    var n := 3; "
    "    var s := \"e=\" + n.ToString(); "
    "    Console.Error.WriteLine(s); "
    "    return s.Length; "               // "e=3".Length = 3
    "  end Main; "
    "end C;";

// (D) Console.Out.Write facet — the stdout TextWriter facet, same lowering.
const char* kConsoleOutFacet =
    "module M; class C "
    "  public static method Main(): int "
    "    var n := 6; "
    "    var s := \"o=\" + n.ToString(); "
    "    Console.Out.Write(s); "
    "    return s.Length; "               // "o=6".Length = 3
    "  end Main; "
    "end C;";

// (E) The SAME local written MULTIPLE times. Every use is a borrow; the single +1
// must be released EXACTLY ONCE at scope end — a leak (no release) OR a double-free
// (a release per write) both flip r.ran. On base all three writes escaped s → leak.
const char* kMultipleWritesSameLocal =
    "module M; class C "
    "  public static method Main(): int "
    "    var s := \"v=\" + 7.ToString(); "
    "    Console.WriteLine(s); "
    "    Console.WriteLine(s); "
    "    Console.Write(s); "
    "    return s.Length; "               // "v=7".Length = 3, freed once
    "  end Main; "
    "end C;";

// (H) A reassigned producer local (#1997 path) that is also WriteLine'd. The OLD
// value is released on the `:=` overwrite (lowerStringLocalReassign); the FINAL
// value leaked on base because the WriteLine argument escaped it. With the fix the
// final value is released once at scope end. Confirms the #1994 relaxation composes
// with the #1997 reassign-release without a double-free.
const char* kReassignedLocalThenWriteLine =
    "module M; class C "
    "  public static method Main(): int "
    "    var a := \"p=\" + 1.ToString(); "
    "    Console.WriteLine(a); "
    "    a := \"q=\" + 2.ToString(); "    // old "p=1" released; new "q=2" must not leak
    "    Console.WriteLine(a); "
    "    return a.Length; "               // "q=2".Length = 3
    "  end Main; "
    "end C;";

// =====================================================================
// DOUBLE-FREE / TRANSFER GUARDS — LSan-clean BOTH before and after the fix.
// =====================================================================

// (F) A producer local that is WriteLine'd AND ESCAPES via `return`. The return
// transfers the local's +1 to the caller, so the local must STAY excluded from the
// scope-end release even though it is also WriteLine'd — otherwise the returned
// reference would be double-freed (or the caller reads freed memory). The #1994
// relaxation only relaxes the WriteLine argument position; the independent
// return-escape keeps s tainted. Clean both before and after the fix. Main consumes
// the returned "v=5".
const char* kWriteLineThenReturnTransfer =
    "module M; class C "
    "  public static method Emit(): string "
    "    var s := \"v=\" + 5.ToString(); "
    "    Console.WriteLine(s); "
    "    return s; "                       // escape: +1 transferred out, not released here
    "  end Emit; "
    "  public static method Main(): int "
    "    var r := C.Emit(); "
    "    return r.Length; "                // "v=5".Length = 3, freed once in Main
    "  end Main; "
    "end C;";

// (G) DIRECT producer arguments (no local): a literal, an interpolation, and a
// concat handed straight to WriteLine. The literal is immortal; the fresh +1
// interpolation/concat are released by the #1031 direct-producer path. The #1994
// relaxation is scoped to a bare string-LOCAL identifier argument, so it does not
// touch these — they stay correct (no leak, no double-free) exactly as on base.
const char* kDirectProducerArgs =
    "module M; class C "
    "  public static method Main(): int "
    "    var n := 7; "
    "    Console.WriteLine(\"hi\"); "
    "    Console.WriteLine($\"n={n}\"); "
    "    Console.WriteLine(\"a\" + n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// LEAK REPROS — fail-without (base leaks) / pass-with (fix releases once).
// §2.1 / §4.6 / §7.4 / §28.3 / §28.9 (v0.197).
// =====================================================================
BEZEL_TEST(console_writeline_string_local_leak_1994,
           user_method_return_local_writeline_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kUserMethodReturnLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // producer +1 released once (leaked pre-fix)
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "n=7".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           concat_local_write_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(kConcatLocalWrite);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // Console.Write (no newline) borrows too
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "v=4".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           console_error_facet_local_lsan_clean_spec_2_1) {
    JitResult r = runAotChecked(kConsoleErrorFacet);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // Console.Error.WriteLine facet borrows
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "e=3".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           console_out_facet_local_lsan_clean_spec_7_4) {
    JitResult r = runAotChecked(kConsoleOutFacet);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // Console.Out.Write facet borrows
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "o=6".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           multiple_writes_same_local_released_once_spec_28_3) {
    JitResult r = runAotChecked(kMultipleWritesSameLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // single +1 freed once — no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "v=7".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           reassigned_local_then_writeline_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kReassignedLocalThenWriteLine);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // composes with #1997 reassign-release, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "q=2".Length
}

// =====================================================================
// DOUBLE-FREE / TRANSFER GUARDS — LSan-clean BOTH before and after the fix.
// §28.1 / §28.3 / §28.9 (v0.197).
// =====================================================================
BEZEL_TEST(console_writeline_string_local_leak_1994,
           writeline_then_return_transfer_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kWriteLineThenReturnTransfer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // WriteLine'd + returned: +1 transferred, not double-freed
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "v=5".Length
}

BEZEL_TEST(console_writeline_string_local_leak_1994,
           direct_producer_args_untouched_spec_28_9) {
    JitResult r = runAotChecked(kDirectProducerArgs);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // literal + fresh interpolation/concat: #1031 path intact
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

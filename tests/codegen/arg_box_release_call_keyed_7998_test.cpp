// Codegen — §8.6 / §28.9 / §28.3 / #7998: the §8.6 ARGUMENT BOX's release is
// keyed to the CALL that minted it, not to the emitting FUNCTION.
//
// THE DEFECT (#7998, MEASURED on the base 82bb7eb3). The pending-release queue
// that holds an argument-position §8.6 box was EMITTER-WIDE: a flat list of
// boxes for the whole emitting function, drained WHOLESALE by whichever call
// reached a discharge point first. When a generic callee is INLINED into the
// caller (emitInlineGenericMethod "emits into the CALLER's own function"), the
// callee's body — and any call inside it — sits BETWEEN the outer call's
// argument mint and the outer call's post-call point. The NESTED call's
// discharge therefore released the OUTER call's box while the outer callee was
// still reading it: a use-after-free, and (because glibc's free() overwrites
// the box's first word, so the `as` type test on the freed box always fails)
// a SILENTLY WRONG VALUE.
//
//   base, p15 (nested GENERIC call):  the artifact path is REJECTED —
//     "Referring to an instruction in another function!" / "IR verification
//     failed (malformed IR emitted)".  The rejected attempt 0ad55442 turned
//     that compile error into a use-after-free + the wrong value (7, not 12).
//   base, p16 (nested ORDINARY call):  the program compiles, prints 7 (the
//     correct answer is 12), and valgrind reports
//     "Invalid read of size 8 ... 0 bytes inside of a block of size 24
//     free'd ... by bz_box_release".
//
// THE FIX. One record PER CALL. A call opens its record before its arguments
// are built; buildOrderedArgs puts the boxes it mints into THAT record; the
// call's own post-call point discharges exactly that record and pops it. The
// records are a stack, so a nested call's record sits above its caller's and
// is popped first: an inner call's discharge can never reach an outer call's
// still-live box.
//
// §8.6 (v0.240): "When a value type is used where `Object` (or an interface
//   implemented by the value type) is expected, the value is boxed: a
//   heap-allocated wrapper holds the value. Boxed values are runtime-managed
//   ([§28.9])."
// §28.9 (v0.240): "Boxed value types ... are exempt from manual disposal ...
//   the type's lifetime is managed by the runtime without user involvement" —
//   which exempts the USER from writing a disposal, NOT the compiler from
//   performing one.
// §28.3 (v0.240): an unbound owned temporary "is disposed at the end of the
//   full expression in which it is produced, after every use of it within
//   that expression has completed".
//
// ⚠ ORACLE DISCIPLINE — WHY THERE IS A LIFETIME ROW AND NOT ONLY A VALUE ROW.
// The `as` test on a FREED box reads the box's first word, and glibc's free()
// has already overwritten it, so the type test fails and the program computes
// a wrong number. That is a VALUE observable, and it is asserted — but a
// value alone cannot see a release that is merely TOO EARLY when the value
// happens to survive. The lifetime rows below therefore make the generated
// code READ the box's first word through a VIRTUAL DISPATCH (`x.GetHashCode()`)
// AFTER the nested call: that read is an instrumented load in the generated
// module, so the repo's own ASan leg (runAotChecked) reports
// "heap-use-after-free" when the box was released early, and reports nothing
// when it was not. MEASURED: on the base, `x.GetHashCode()` after the nested
// ordinary call yields, under ASan,
//
//   ERROR: AddressSanitizer: heap-use-after-free on address 0x...048
//   READ of size 8 at 0x...048 thread T0
//     #0 main
//   0x...048 is located 8 bytes inside of 24-byte region [0x...040,0x...058)
//   freed by thread T0 here:
//     #1 bz_box_release build/../src/runtime/bezel_runtime_refcounting.cpp:722
//
// while the `as`-only shape prints 7 and ASan says nothing (the freed box's
// first word then looks like a string descriptor, which routes the `as` test
// down the `isa.guarded` arm and away from the instrumented load). Valgrind
// catches both shapes; ASan catches the dispatch shape.
//
// ⚠ SCOPE. This fix changes the DISCHARGE SITE only. The callee-OWNED box (the
// callee READS the parameter, so the escape analysis suppresses the
// caller-side release and nobody disposes the box — a 24-byte leak per call)
// is a separate defect, filed as issue #8011, and is NOT changed here. The
// rows below do not depend on it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ── The programs ────────────────────────────────────────────────────────────
//
// ⚠ WHY EVERY FORMAL BELOW IS SPELLED `System.Object` (§8.1, §2.3, §2.7,
// §8.6, v0.350). THE §8.6 ARGUMENT BOX is minted for a value-typed argument
// bound to a formal whose type is the ROOT `Object` (or an interface the
// value type implements) — §8.6: "When a value type is used where `Object`
// (or an interface implemented by the value type) is expected, the value is
// **boxed**: a heap-allocated wrapper holds the value."
//
// A BARE `Object` in a formal's type is NOT the root where a TYPE PARAMETER
// named `Object` shadows it. §8.1's *Definition*: "a nested type or a type
// parameter named `Object` shadows the root under §2.7 as any declaration
// shadows an always-available name, and `System.Object` still reaches it";
// §2.3's "Core types" row says the same ("a nested type or type parameter
// named `Object` shadows it, and `System.Object` still reaches it"). So in
// `Tag<Object>(x: Object)` the formal's type is the TYPE PARAMETER, the `int`
// argument is admitted only as the substituted value type (§5.1), and NO §8.6
// box is minted at all — the programs would not exercise the defect this
// file covers, and the two rows that read the box back would be VACUOUS.
//
// ⇒ THE FORMALS BELOW NAME THE ROOT EXPLICITLY: `x: System.Object`. §8.1
// says the qualified spelling "still reaches it", so the formal's type IS the
// root `Object` even though the enclosing method declares a type parameter
// named `Object` — §8.6's box is minted, the callee reads the box's payload,
// and the rows below measure the defect they were written for. (The type
// parameter is still named `Object` on purpose: the programs then also pin
// that the qualified spelling reaches the root INSIDE a scope where the bare
// name is shadowed.)

// (1) THE ISSUE'S OWN REPRO, with the box read back so the row's value is not
// a constant. `Tag<Object>` declares ONE type parameter, named `Object`; the
// call supplies `int`. The formal is the ROOT `Object` (spelled
// `System.Object` — see the note above), so the `int` argument is boxed to it
// (§8.6). The box is minted in the CALLER (Main) and must be released there,
// after the call returns.
//
// On the base the box stayed queued: the ARTIFACT path (`bezelc -c`, §2.4 —
// the path the issue was reported on) is rejected with "Referring to an
// instruction in another function!", and the EXECUTABLE path never releases
// the box at all (a 24-byte leak per call). The loop makes the leak
// decisive under LSan (20 x 24 B), as CLAUDE.md's "Vacuous-oracle traps"
// requires of a leak repro.
//
// The sum is 20 x 5 = 100: the box's payload, read back through §8.6's box.
const char* kIssueRepro =
    "module M7998R; "
    "public class Program "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      t := t + Tag<int>(5); "
    "      k := k + 1; "
    "    end while; "
    "    if t = 100 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "  public static method Tag<Object>(x: System.Object): int "
    "    => (x as int) ?? 0; "
    "end Program;";

// (2) THE F1 SHAPE — a nested GENERIC call inside the inlined generic callee.
// The callee READS the boxed parameter (`x as int` is a payload copy, NOT an
// escape — §8.6) AFTER the nested call, so the nested call's post-call
// discharge point sits between the mint and the read. The nested call is
// `Helper<Object>(5)`, which is itself generic and mints a second box.
//
// The correct answer is 7 (Helper) + 5 (the box) = 12. The base REJECTS this
// program outright ("Referring to an instruction in another function!"), so
// the row fails without the fix on the COMPILE; the rejected attempt 0ad55442
// compiled it and then produced 7 (a use-after-free) — strictly worse than the
// base, which is why this shape is the one the fix had to get right.
const char* kNestedGenericExpression =
    "module M7998G; "
    "public class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(Tag<int>(5)); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag<Object>(x: System.Object): int "
    "    => Helper<Object>(5) + ((x as int) ?? 0); "
    "  public static method Helper<Object>(o: System.Object): int => 7; "
    "end Program;";

// (3) THE F2 SHAPE — the ordinary-nested-call twin of (2). The nested call is
// a non-generic method, so the base's ordinary-path discharge already
// released the OUTER call's box while it was still live: the program compiled,
// printed 7 (the correct answer is 12), and valgrind reported the
// use-after-free. This is the PRE-EXISTING half of the defect (same root
// cause), and the fix closes it with the same one-line change of shape.
const char* kNestedOrdinaryExpression =
    "module M7998O; "
    "public class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(Tag<int>(5)); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag<Object>(x: System.Object): int "
    "    => Helper() + ((x as int) ?? 0); "
    "  public static method Helper(): int => 7; "
    "end Program;";

// (4) THE LIFETIME TWIN of (3) — the same shape, plus a VIRTUAL DISPATCH on
// the boxed parameter AFTER the nested call. `x.GetHashCode()` reads the box's
// first word (the vtable slot) through an INSTRUMENTED load in the generated
// module, so the repo's ASan leg reports "heap-use-after-free" if the box was
// released early. The two dispatches cancel, so the printed value stays 12 and
// the row asserts a NUMBER (not "it did not crash"). The callee READS the
// parameter only (a receiver read is not an escape — the escape analysis
// keeps the box caller-owned), so the box IS released — by the CALL that minted
// it, at that call's post-call point.
const char* kNestedOrdinaryExpressionLifetime =
    "module M7998L; "
    "public class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(Tag<int>(5)); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag<Object>(x: System.Object): int "
    "    => Helper() + ((x as int) ?? 0) "
    "       + (x.GetHashCode() - x.GetHashCode()); "
    "  public static method Helper(): int => 7; "
    "end Program;";

// (5) THE LIFETIME TWIN of (2) — the nested GENERIC call's twin of (4).
const char* kNestedGenericExpressionLifetime =
    "module M7998K; "
    "public class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(Tag<int>(5)); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag<Object>(x: System.Object): int "
    "    => Helper<Object>(5) + ((x as int) ?? 0) "
    "       + (x.GetHashCode() - x.GetHashCode()); "
    "  public static method Helper<Object>(o: System.Object): int => 7; "
    "end Program;";

}  // namespace

// =====================================================================
// (A) THE ISSUE'S OWN REPRO — the §8.6 box minted in the CALLER must be
// released by the CALLER, after the call returns. On the base the box was
// never released (a 24-byte leak per call; the artifact path was rejected
// outright with "Referring to an instruction in another function!").
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.240
BEZEL_TEST(codegen_arg_box_release_call_keyed_7998,
           the_issues_own_repro_releases_the_box_in_the_caller_spec_8_6) {
    JitResult plain = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    // The LIFETIME half: LSan must see no leak. On the base the box was
    // queued and never released — 20 x 24 B.
    JitResult checked = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 41);
}

// =====================================================================
// (B) THE F1 SHAPE — a nested GENERIC call inside the inlined generic
// callee. The outer box must still be ALIVE at the read that follows the
// nested call, and must be released after it.
// =====================================================================

// per §28.3 — the box's lifetime ends at the end of the full expression,
// AFTER every use of it within that expression has completed.
BEZEL_TEST(codegen_arg_box_release_call_keyed_7998,
           nested_generic_call_leaves_the_outer_box_alive_spec_28_3) {
    JitResult plain = runAot(kNestedGenericExpression);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "12\n");
    JitResult checked = runAotChecked(kNestedGenericExpression);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.stdoutText, "12\n");
}

// =====================================================================
// (C) THE F2 SHAPE — the ordinary-nested-call twin. PRE-EXISTING at the
// named baseline, same root cause: the nested call's discharge released the
// OUTER call's still-live box (use-after-free + the value 7 instead of 12).
// =====================================================================

// per §28.3 — the same boundary, for the ordinary nested call.
BEZEL_TEST(codegen_arg_box_release_call_keyed_7998,
           nested_ordinary_call_leaves_the_outer_box_alive_spec_28_3) {
    JitResult plain = runAot(kNestedOrdinaryExpression);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, "12\n");
    JitResult checked = runAotChecked(kNestedOrdinaryExpression);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.stdoutText, "12\n");
}

// =====================================================================
// (D) THE LIFETIME ROWS — the value alone cannot see a release that is
// merely TOO EARLY, so these two rows make the generated code READ the box's
// first word through a virtual dispatch AFTER the nested call. On the base
// the ASan leg reports "heap-use-after-free ... freed by bz_box_release";
// with the fix it reports nothing.
// =====================================================================

// per §28.3 / §28.9 — the box is released by the CALL that minted it, after
// the callee's last use of it.
BEZEL_TEST(codegen_arg_box_release_call_keyed_7998,
           nested_ordinary_call_has_no_use_after_free_spec_28_9) {
    JitResult checked = runAotChecked(kNestedOrdinaryExpressionLifetime);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: AddressSanitizer: heap-use-after-free
    BEZEL_EXPECT_EQ(checked.stdoutText, "12\n");
}

// per §28.3 / §28.9 — the nested GENERIC call's twin of the row above.
BEZEL_TEST(codegen_arg_box_release_call_keyed_7998,
           nested_generic_call_has_no_use_after_free_spec_28_9) {
    JitResult checked = runAotChecked(kNestedGenericExpressionLifetime);
    BEZEL_EXPECT_TRUE(checked.ran);   // pre-fix: the artifact path is rejected outright
    BEZEL_EXPECT_EQ(checked.stdoutText, "12\n");
}

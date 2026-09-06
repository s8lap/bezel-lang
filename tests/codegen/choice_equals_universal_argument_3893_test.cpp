// fix #3893 — `choiceValue.Equals(x)` mishandled every argument that is not
// itself a value of the receiver's own choice type (spec §8.1 *Definition*,
// §8.3 *`Equals` and `GetHashCode`*, §35.7 Equality, hashing, `ToString` — v0.294). This is the
// choice sibling of fix #3538 (record/enum), which #3893's audit found had
// never received the same gate: `LlvmBackend_Calls2.cpp`'s Choice branch of
// `lowerValueSemMethodOn` went straight from lowering the argument to
// `if (other->getType()->isPointerTy()) other = CreateLoad(st, other);` where
// `st` is THIS choice's own struct type — with no check that the argument's
// static type was ever `ts` (this choice) at all.
//
// §8.1 declares exactly one universal member (`Equals(other: Object?): bool`),
// so sema admits an argument of ANY type, and §8.3 says a choice compares
// equal only to another value of the SAME choice type carrying the same active
// case and equal payloads. The pre-fix arm instead force-loaded `other`
// through `st` whenever the LOWERED value happened to be a pointer, and fed it
// to `emitChoiceEquality` whenever it happened to be ANY struct — regardless
// of its real static type.
//
// ── MEASURED on the pre-fix tree (this worktree, before the #3893 fix) ───────
//
// `record Pair(A: int, B: int); choice Shape Circle(radius: int), Square
// (side: int); end Shape;` with `s := Shape.Circle(1)`:
//
//   s.Equals(new Pair(1, 2))
//        `bezelc` itself SIGSEGVs during codegen — confirmed via gdb backtrace:
//        `emitChoiceEquality`'s `CreateExtractValue` (LlvmBackend_Layout.cpp)
//        indexes into `other` (Pair's 2-field struct) using an index derived
//        from Shape's case-payload layout; the out-of-bounds extractvalue
//        crashes LLVM's IRBuilder internally. A COMPILE-TIME crash — the
//        strictly worse cousin of #3889/#3891's compile-clean-then-segfault.
//   s.Equals(42)
//        compiles, but with a scary #2434 "internal: lowerLocalDecl's
//        initializer expression failed to lower to a value for this local's
//        slot (degraded to a zero-initialized value)" note — the arm returned
//        null (an `int` is neither a pointer nor a struct) instead of the
//        correct static False, so the answer was right by ACCIDENT (zero
//        happens to mean false) rather than by any decision the compiler made.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Reuse fix #3538's own both-arms predicates — `equalsArgSettlesFalse` /
// `equalsArgIsOwnTypeOrUnresolved` (LlvmBackend_impl.hpp) — instead of
// inventing a choice-specific copy (CLAUDE.md §6): a value primitive, `()`, or
// a different record/enum/choice settles False directly (§8.3, argument still
// evaluated per §22.5); this choice's own type (or an unresolved generic) is
// the pre-existing structural-equality path; everything else (`Object`, an
// interface, a class, `Shape?`) routes through the SAME run-time
// `lowerValueSemUniversalEquals` the record/enum arms use. `isBoxableValueType`
// does not yet cover Choice (§35.8's "value-like choices box per the general
// rule" is unimplemented there), so that residual run-time path currently
// declines for a choice receiver instead of dispatching — a PRE-EXISTING gap,
// not a new regression (filed separately as #3900); it is strictly safer than
// the wild pointer read it replaces, so it is intentionally NOT exercised here
// (a `.Equals(Object)` row would print nothing rather than "False", which
// belongs to the follow-up issue's own test, not this one).
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  the audit's repro: same-choice-same-case (True), same-choice-
//       different-case (False, pre-existing/untouched), and every settles-
//       False argument shape — value primitives, a different record, a
//       different enum, and a different choice.
//   T2  §22.5: the statically-False arm still evaluates its argument exactly
//       once, and BEFORE the printed answer.
//   T3  §8.3's hash/equals control: two equal Shape values hash equal — this
//       path is untouched by the fix, exercised here to confirm the new gate
//       does not disturb it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE AUDIT'S REPRO (fix #3893). Pre-fix, row 06 crashed `bezelc`
// itself (never reached "END"); row 03 compiled but only by accident (see
// file header). Asserting the WHOLE stdout — not just `r.ran`/`exitCode` —
// is what catches both a crashed compile (stdout never populated, `r.ran`
// false) and an accidentally-right answer sitting next to a wrong one.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_equals_non_choice_and_other_choice_arguments_spec_8_1_8_3) {
    auto r = runAot(
        "module M3893A; "
        "record Pair(A: int, B: int); "
        "enum Color Red, Green, Blue; end Color; "
        "choice Shape Circle(radius: int), Square(side: int); end Shape; "
        "choice Direction North, South; end Direction; "
        "class C "
        "  public static method Main(): int "
        "    var c1: Shape := Shape.Circle(1); "
        "    var c2: Shape := Shape.Circle(1); "
        "    var sq: Shape := Shape.Square(1); "
        "    var p: Pair := new Pair(1, 2); "
        "    Console.WriteLine(\"01 \" + c1.Equals(c2).ToString()); "
        "    Console.WriteLine(\"02 \" + c1.Equals(sq).ToString()); "
        "    Console.WriteLine(\"03 \" + c1.Equals(42).ToString()); "
        "    Console.WriteLine(\"04 \" + c1.Equals(true).ToString()); "
        "    Console.WriteLine(\"05 \" + c1.Equals(\"s\").ToString()); "
        "    Console.WriteLine(\"06 \" + c1.Equals(p).ToString()); "
        "    Console.WriteLine(\"07 \" + c1.Equals(Color.Red).ToString()); "
        "    Console.WriteLine(\"08 \" "
        "      + c1.Equals(Direction.North).ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("01 True\n"
                                "02 False\n"
                                "03 False\n"
                                "04 False\n"
                                "05 False\n"
                                "06 False\n"
                                "07 False\n"
                                "08 False\n"
                                "END\n"));
}

// ── T2 — §22.5: a statically-False `Equals` call still evaluates its
// argument, and evaluates it exactly once, before the call's own answer
// prints. `Bump` prints and returns; its line must precede the row it feeds.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_equals_statically_false_still_evaluates_argument_spec_22_5) {
    auto r = runAot(
        "module M3893B; "
        "choice Shape Circle(radius: int), Square(side: int); end Shape; "
        "class C "
        "  public static method Bump(n: int): int "
        "    Console.WriteLine(\"side \" + n.ToString()); "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var c: Shape := Shape.Circle(5); "
        "    Console.WriteLine(\"r1 \" + c.Equals(C.Bump(1)).ToString()); "
        "    Console.WriteLine(\"r2 \" + c.Equals(C.Bump(2)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("side 1\nr1 False\nside 2\nr2 False\n"));
}

// ── T3 — CONTROL: §8.3's "equal values hash equal" for two same-case,
// equal-payload Shape values. This route (own-type structural equality /
// GetHashCode) is untouched by the fix; pinned here to confirm the new
// argument-type gate does not disturb it.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_own_type_equals_and_hash_control_spec_8_3) {
    auto r = runAot(
        "module M3893C; "
        "choice Shape Circle(radius: int), Square(side: int); end Shape; "
        "class C "
        "  public static method Main(): int "
        "    var a: Shape := Shape.Circle(5); "
        "    var b: Shape := Shape.Circle(5); "
        "    Console.WriteLine(\"eq \" + a.Equals(b).ToString()); "
        "    Console.WriteLine(\"hash \" "
        "      + (a.GetHashCode() = b.GetHashCode()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\nhash True\n"));
}

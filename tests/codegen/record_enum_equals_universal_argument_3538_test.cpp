// fix #3538 — `recordValue.Equals(x)` / `enumValue.Equals(x)` mishandled every
// argument that is not itself a value of the receiver's own type (spec §8.1
// *Definition*, §8.3 *`Equals` and `GetHashCode`*, §8.6 *Boxing*, §22.5
// *Evaluation order* — v0.283).
//
// §8.1 declares exactly one universal member:
//
//     public virtual method Equals(other: Object?): bool;
//
// so sema admits an argument of ANY type, §8.6 boxes a value-typed one at that
// `Object` location, and §8.3's record row ("field-by-field value equality") and
// enum row ("underlying integer comparison") compare the receiver against a
// value OF ITS OWN TYPE. The codegen arms recognised only one shape each and
// mis-answered the rest in four different ways.
//
// ── MEASURED ON b43196a, all through the real driver ─────────────────────────
//
// `record Pt(X: int, Y: int); record Other(A: int);` with `p := new Pt(1, 2)`:
//
//   p.Equals(7) / (true) / (3.5) / ('a') / (Color.Red)
//        the ENCLOSING STATEMENT IS DROPPED WHOLE. `bezelc` exits 0 with a
//        `Severity::Warning` #3079 note, and `Console.WriteLine(p.Equals(7)
//        .ToString());` printed NOTHING — not even the line terminator. This is
//        the filed defect: a silent missing line, not a crash.
//   p.Equals(new Other(1))
//        `bezelc` SEGV, rc=139, no diagnostic — a crash the filed issue did not
//        know about.
//   p.Equals(o), `o: Object` holding a box of a Pt EQUAL to p
//        False. §8.3 says True. A SILENT WRONG ANSWER, the worst of the four:
//        the arm did `CreateLoad(recordStruct, other)` straight off the BOX
//        POINTER, so it compared `p` against the box's vtable and refcount
//        words. The same load is why `p.Equals("s")` answered False by reading
//        a string's header bytes as `X`/`Y`.
//
// `enum Color Red, Green, Blue; end Color;` with `c := Color.Green`, and a
// second `enum Size Small, Med; end Size;`:
//
//   c.Equals(1)         True   — an `int` is not a `Color`
//   c.Equals(true)      True   — `i1 1` zero-extends onto Green's ordinal
//   c.Equals(Size.Med)  True   — a DIFFERENT enum type, same ordinal
//   c.Equals("s") / (a boxed anything) / (a record)
//                       `bezelc` rc=1, "ZExt only operates on integer"
// Three more silent wrong answers — and these are the direction that matters
// most, because `True` is the answer a program acts on.
//
// ── THE FIX, AND WHY IT IS NOT A NEW PREDICATE ───────────────────────────────
//
// Each arm now partitions the argument by its STATIC type into exactly three
// cases: this receiver's own type (the existing structural / integer compare);
// a type §8.3 settles at False on its own (`int`, `bool`, `double`, `char`,
// `string`, `()`, a different record/enum/choice) — answered `False` with the
// argument still LOWERED, because §22.5 makes its effects part of the call
// whatever the answer is; and everything else (`Object`, an interface, a class,
// any nullable form), which can dynamically BE a box of the receiver's type and
// so is answered by boxing both sides and dispatching through
// `lowerObjectEquals` — the SAME route the reference-semantics receiver has
// taken since #3443, whose box-vtable `Equals` already implements §8.3's rule.
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  the issue's repro: the four value primitives, in BOTH directions where
//       the receiver's own type is involved, plus the `string` argument that
//       "already worked" and in fact worked by a wild read.
//   T2  the ENUM arm's silent `True`s — the rows that must FLIP, not merely
//       start compiling. Without them a fix that only stopped the malformed IR
//       would pass.
//   T3  the `Object` argument, both directions, for both receiver kinds — the
//       run-time route, and the row that fails on a fix that answers `False`
//       unconditionally.
//   T4  §8.3's stated invariant across the boxed and unboxed spellings, now
//       that T3 makes `p.Equals(o)` true.
//   T5  §22.5: the argument of a statically-`False` call still evaluates.
//   T6  BOTH-SIDES CONTROL — the primitive receiver's own `Equals`, untouched
//       by this fix, passing identically on both sides.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE ISSUE'S REPRO. Pre-fix rows 03..07 printed NO LINE AT ALL (the
// statement was dropped) and row 08 crashed the compiler outright, so the
// program's stdout was four lines short. Asserting the whole stdout — rather
// than "it ran" — is what makes a dropped line fail.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           record_equals_value_primitive_arguments_spec_8_1_8_6) {
    auto r = runAot(
        "module M3538A; "
        "record Pt(X: int, Y: int); "
        "record Other(A: int); "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(1, 2); "
        "    var q: Pt := new Pt(1, 2); "
        "    var d: Pt := new Pt(9, 9); "
        "    Console.WriteLine(\"01 \" + p.Equals(q).ToString()); "
        "    Console.WriteLine(\"02 \" + p.Equals(d).ToString()); "
        "    Console.WriteLine(\"03 \" + p.Equals(7).ToString()); "
        "    Console.WriteLine(\"04 \" + p.Equals(true).ToString()); "
        "    Console.WriteLine(\"05 \" + p.Equals(3.5).ToString()); "
        "    Console.WriteLine(\"06 \" + p.Equals('a').ToString()); "
        "    Console.WriteLine(\"07 \" + p.Equals(\"s\").ToString()); "
        "    Console.WriteLine(\"08 \" + p.Equals(new Other(1)).ToString()); "
        "    Console.WriteLine(\"09 \" + p.Equals(Color.Red).ToString()); "
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
                                "09 False\n"
                                "END\n"));
}

// ── T2 — THE ENUM ARM'S SILENT `True`s.
//
// Rows 03/04/05 answered **True** on b43196a and must now answer False; rows
// 06/07 were "ZExt only operates on integer" (rc=1, no binary). This is the
// direction of the fix that a "make it stop failing" change would get wrong:
// only asserting the FLIPPED value catches it. Rows 01/02 pin that the enum's
// own §8.3 row is untouched.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           enum_equals_non_enum_arguments_are_false_spec_8_1_8_3) {
    auto r = runAot(
        "module M3538B; "
        "record Pt(X: int, Y: int); "
        "enum Color Red, Green, Blue; end Color; "
        "enum Size Small, Med; end Size; "
        "class C "
        "  public static method Main(): int "
        "    var c: Color := Color.Green; "
        "    var s: string := \"s\"; "
        "    Console.WriteLine(\"01 \" + c.Equals(Color.Green).ToString()); "
        "    Console.WriteLine(\"02 \" + c.Equals(Color.Blue).ToString()); "
        "    Console.WriteLine(\"03 \" + c.Equals(1).ToString()); "
        "    Console.WriteLine(\"04 \" + c.Equals(true).ToString()); "
        "    Console.WriteLine(\"05 \" + c.Equals(Size.Med).ToString()); "
        "    Console.WriteLine(\"06 \" + c.Equals(s).ToString()); "
        "    Console.WriteLine(\"07 \" + c.Equals(new Pt(1, 2)).ToString()); "
        "    Console.WriteLine(\"08 \" + c.Equals(3.5).ToString()); "
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

// ── T3 — THE `Object` ARGUMENT, THE RUN-TIME ROUTE.
//
// Rows 01 and 05 are the ones the old code got WRONG rather than merely
// dropped: an `Object` holding a box of a value equal to the receiver. §8.6
// boxes a value at an `Object` location and §8.3 compares the payloads, so the
// answer is True. Rows 02/03/06/07 are the same route's False direction — a box
// of a different value, and a box of an unrelated type — so a fix that answers
// True unconditionally dies here too.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           equals_boxed_object_argument_both_directions_spec_8_3_8_6) {
    auto r = runAot(
        "module M3538C; "
        "record Pt(X: int, Y: int); "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(1, 2); "
        "    var same: Object := new Pt(1, 2); "
        "    var diff: Object := new Pt(3, 4); "
        "    var num: Object := 7; "
        "    var c: Color := Color.Green; "
        "    var ec: Object := Color.Green; "
        "    var ed: Object := Color.Blue; "
        "    Console.WriteLine(\"01 \" + p.Equals(same).ToString()); "
        "    Console.WriteLine(\"02 \" + p.Equals(diff).ToString()); "
        "    Console.WriteLine(\"03 \" + p.Equals(num).ToString()); "
        "    Console.WriteLine(\"04 \" + p.Equals(ec).ToString()); "
        "    Console.WriteLine(\"05 \" + c.Equals(ec).ToString()); "
        "    Console.WriteLine(\"06 \" + c.Equals(ed).ToString()); "
        "    Console.WriteLine(\"07 \" + c.Equals(num).ToString()); "
        "    Console.WriteLine(\"08 \" + c.Equals(same).ToString()); "
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
                                "05 True\n"
                                "06 False\n"
                                "07 False\n"
                                "08 False\n"
                                "END\n"));
}

// ── T4 — §8.3's INVARIANT, now that T3's row 01 is True.
//
// "`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() =
// b.GetHashCode()`." The unboxed receiver, the boxed argument it now reports
// itself equal to, and a third equal value reached through a `Pt` local must
// all hash alike. The non-zero assertion matters because 0 is what every
// dropped/declined path already produces (#2804), so `h = 0` would satisfy the
// equalities vacuously.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           equals_implies_equal_hash_across_boxed_and_unboxed_spec_8_3) {
    auto r = runAot(
        "module M3538D; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(1, 2); "
        "    var q: Pt := new Pt(1, 2); "
        "    var o: Object := new Pt(1, 2); "
        "    Console.WriteLine(\"eq \" + p.Equals(o).ToString()); "
        "    Console.WriteLine(\"hp \" "
        "      + (p.GetHashCode() = q.GetHashCode()).ToString()); "
        "    Console.WriteLine(\"ho \" "
        "      + (p.GetHashCode() = o.GetHashCode()).ToString()); "
        "    Console.WriteLine(\"nz \" "
        "      + (p.GetHashCode() <> 0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("eq True\nhp True\nho True\nnz True\n"));
}

// ── T5 — §22.5. The statically-`False` arm answers without needing the
// argument's value, so it would be easy to answer without EVALUATING it. §22.5
// makes the argument's effects part of the call, and the reference-receiver
// route (#3443) lowers its argument unconditionally, so the two spellings must
// agree about this too. `Bump` prints and returns, and its line must appear
// BEFORE the answer's — once per call, not twice.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           statically_false_equals_still_evaluates_argument_spec_22_5) {
    auto r = runAot(
        "module M3538E; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Bump(n: int): int "
        "    Console.WriteLine(\"side \" + n.ToString()); "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(1, 2); "
        "    Console.WriteLine(\"r1 \" + p.Equals(C.Bump(1)).ToString()); "
        "    Console.WriteLine(\"r2 \" + p.Equals(C.Bump(2)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("side 1\nr1 False\nside 2\nr2 False\n"));
}

// ── T6 — BOTH-SIDES CONTROL. The PRIMITIVE receiver's `Equals`/`GetHashCode`
// arm sits directly above the two this fix rewrites, in the same function, and
// is deliberately not touched. This row exercises it in both directions and
// passes identically on both sides of the fix — so a failure here means the
// file or the harness broke, not that the fix stopped working.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           control_primitive_receiver_equals_unaffected_spec_8_3) {
    auto r = runAot(
        "module M3538F; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 5; "
        "    var d: double := 2.5; "
        "    var b: bool := true; "
        "    Console.WriteLine(\"01 \" + i.Equals(5).ToString()); "
        "    Console.WriteLine(\"02 \" + i.Equals(6).ToString()); "
        "    Console.WriteLine(\"03 \" + d.Equals(2.5).ToString()); "
        "    Console.WriteLine(\"04 \" + d.Equals(2.75).ToString()); "
        "    Console.WriteLine(\"05 \" + b.Equals(true).ToString()); "
        "    Console.WriteLine(\"06 \" + (i.GetHashCode() = 5).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("01 True\n"
                                "02 False\n"
                                "03 True\n"
                                "04 False\n"
                                "05 True\n"
                                "06 True\n"));
}

// ── T7 (fix #3918, §8.1/§9.1/§9.2, v0.294) — the RECORD twin of fix #3913's
// choice-arm SIGSEGV. `p.Equals(arg)` where `p`'s receiver type is a record
// and `arg`'s STATIC type is an UNRESOLVED generic method type parameter
// reaches `equalsArgIsOwnTypeOrUnresolved`'s "or unresolved" clause, which
// (per its own documented limitation — codegen substitutes VALUES, never
// `ctx_.exprType`'s answer) cannot see what the call site actually
// substitutes the parameter to. Pre-fix, the record arm assumed any
// STRUCT-shaped `other` must be `ts`-shaped and fed it straight to
// `emitStructEquality`, which unconditionally `CreateExtractValue`s at
// `ts`'s own field indices.
//
// MEASURED via gdb on the pre-fix tree: `Foo<Pair>(p, new Pair(9))` with
// `p: Pt` (a THREE-field record; `Pair` has ONE field) SIGSEGVs `bezelc`
// itself — fault in `llvm::Value::setName`, called from
// `ExtractValueInst::Create` inside `emitStructEquality`
// (LlvmBackend_Layout.cpp:6227), reached through `lowerValueSemMethodOn`'s
// record arm (LlvmBackend_Calls2.cpp) — the identical backtrace shape #3913
// reported for the choice arm's `emitChoiceEquality`.
//
// Post-fix, the mismatched-shape guard added for #3918 fails this CLOSED: a
// located `Severity::Error` diagnostic, and the whole compile is REJECTED
// (`r.ran` is false) rather than the compiler crashing. This is a DIFFERENT
// closing mechanism than #3913's choice-arm fix (which relies on the
// pre-existing §7.5/#3452 null-conditional "declining inner lowering fails
// closed" wrapper): this call site is a PLAIN `.` access, so there is no such
// wrapper to route a bare decline through, and #3918's guard reports the
// error directly instead — mirroring the ENUM arm's own precedent a few
// lines above in the same function for the identical shape of problem,
// rather than falling through to the pre-existing #2491/#3079
// `Severity::Warning` "return-value expression failed to lower" quiet
// degrade (which is what a BARE `return nullptr` here would do, since
// nothing in this call's plain-`.` path fails closed on its own — MEASURED:
// the sibling pre-existing decline three lines above this guard,
// `!other->getType()->isStructTy()`, already exhibits exactly that quiet
// degrade for the same unresolved-parameter shape when it resolves to a
// non-struct type, e.g. `Foo<int>(p, 7)` — filed as issue #3922, out of
// scope for this fix).
//
// A bare not-ran check on its own is deliberately not the oracle here (it is
// also satisfied by a crash or a signal death): the diagnostic TEXT is
// asserted below, which only the fixed-closed outcome produces — the base
// tree's SIGSEGV asserts nothing here at all (whether the run "ran" is
// undefined after a child crash, not merely false), which is exactly the
// FAIL-WITHOUT asymmetry this row exists to prove.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           record_equals_struct_shaped_generic_arg_fails_closed_not_crash_3918) {
    auto r = runAot(
        "module M3538G; "
        "record Pt(X: int, Y: int, Z: int); "
        "record Pair(A: int); "
        "class C "
        "  public static method Foo<Arg>(p: Pt, arg: Arg): bool "
        "    return p.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var p := new Pt(1, 2, 3); "
        "    var b := C.Foo<Pair>(p, new Pair(9)); "
        "    Console.WriteLine(b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("member `Equals`") != std::string::npos &&
            d.message.find("`M3538G.Pt`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
    bool explainsWhy = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not resolve to a value of that record") !=
            std::string::npos)
            explainsWhy = true;
    BEZEL_EXPECT_TRUE(explainsWhy);
}

// ── T8 (fix #3918) — ⚠ FALSE-DECLINE GUARD. T7's guard must fire ONLY for a
// genuinely mismatched instantiation, never for a generic argument that
// resolves to THIS record's own type — that would turn a correct program
// into a spuriously rejected one, the opposite failure from #3918's crash.
// `Foo<Pt>(p, q)` / `Foo<Pt>(p, d)` substitute the SAME unresolved-type-
// parameter path T7 exercises, but at `Arg = Pt` (the receiver's own type),
// so the loaded argument's actual LLVM type IS `st` and the new
// `other->getType() != st` comparison must NOT fire: both calls must still
// reach `emitStructEquality` and answer structurally, exactly as the
// direct (non-generic) `p.Equals(q)` spelling does in T1.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           record_equals_own_type_generic_arg_still_lowers_3918) {
    auto r = runAot(
        "module M3538H; "
        "record Pt(X: int, Y: int, Z: int); "
        "class C "
        "  public static method Foo<Arg>(p: Pt, arg: Arg): bool "
        "    return p.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var p := new Pt(1, 2, 3); "
        "    var q := new Pt(1, 2, 3); "
        "    var d := new Pt(9, 9, 9); "
        "    Console.WriteLine(\"same \" + C.Foo<Pt>(p, q).ToString()); "
        "    Console.WriteLine(\"diff \" + C.Foo<Pt>(p, d).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("same True\ndiff False\n"));
}

// ── T9 (fix #3922, §8.1/§9.1/§9.2, v0.294) — the NON-STRUCT sibling of T7's
// #3918 guard. `p.Equals(arg)` where `p`'s receiver type is a record and
// `arg`'s STATIC type is an UNRESOLVED generic method type parameter reaches
// the SAME `equalsArgIsOwnTypeOrUnresolved` "or unresolved" clause T7 does,
// but THIS instantiation substitutes it to a NON-STRUCT type (`int`) rather
// than an unrelated struct — the pre-existing bare `return nullptr` three
// lines above T7's #3918 guard, present since #3538/#3556 and left untouched
// by #3918 (filed as issue #3922, the sibling gap #3918's own commit
// flagged), not a NEW decline.
//
// MEASURED on HEAD (2760e31, #3918 already landed): `bezelc` on
// `Foo<int>(p, 7)` returning the comparison directly compiles clean (rc=0)
// and PRINTS `False` at run time — the §2491 "return-value expression failed
// to lower … degraded to a zero/null return value" internal note fires, but
// `hasErrors()` stays false and the program runs. `False` happens to be the
// §8.3-correct answer for an `int` against a record (structurally
// impossible to match), which would make a naive `stdoutText == "False"`
// oracle unable to tell "correctly computed" from "defaulted to zero by
// accident" — so this row does NOT use that shape. It instead uses the
// argument-of-a-`Console.WriteLine`-statement shape (the sibling `#3079`
// degrade, same underlying `res == nullptr` cause), which MEASURABLY prints
// NOTHING AT ALL for the failing line while the statements immediately
// before and after it still print — an unambiguous wrong answer: the whole
// comparison silently vanishes, not merely computes a value that happens to
// coincide with the right one.
//
// Post-fix, the new guard added by this commit (mirroring T7's #3918 guard
// and the ENUM arm's own pre-existing `!isIntegerTy()` precedent a few dozen
// lines above both) reports a located `Severity::Error` and rejects the
// build (`r.ran` false) instead of degrading either the return value or the
// statement to nothing.
BEZEL_TEST(codegen_record_enum_equals_universal_argument_3538,
           record_equals_non_struct_generic_arg_fails_closed_not_degraded_3922) {
    auto r = runAot(
        "module M3538I; "
        "record Pt(X: int, Y: int, Z: int); "
        "class C "
        "  public static method Foo<Arg>(p: Pt, arg: Arg) "
        "    Console.WriteLine(\"before\"); "
        "    Console.WriteLine(p.Equals(arg).ToString()); "
        "    Console.WriteLine(\"after\"); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var p := new Pt(1, 2, 3); "
        "    C.Foo<int>(p, 7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("member `Equals`") != std::string::npos &&
            d.message.find("`M3538I.Pt`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
    bool explainsWhy = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not resolve to a value of that record") !=
            std::string::npos)
            explainsWhy = true;
    BEZEL_EXPECT_TRUE(explainsWhy);
}

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
//   T4  fix #3925 (§8.1/§35.4/§35.7, v0.294): the CHOICE twin of fix #3922's
//       record-arm non-struct guard — a PLAIN `.` access whose `Equals`
//       argument is an UNRESOLVED generic method type parameter that THIS
//       call site instantiates to a non-struct type (`int`) now fails
//       closed with a located diagnostic, instead of quietly degrading to
//       the #2491/#3079 warning backstop (`bezelc` exiting 0 while the
//       comparison silently vanishes at run time).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
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

// ── T4 (fix #3925, §8.1/§35.4/§35.7, v0.294) — the CHOICE sibling of
// `record_enum_equals_universal_argument_3538_test.cpp`'s T9 (fix #3922).
// `s.Equals(arg)` where `s`'s receiver type is a choice and `arg`'s STATIC
// type is an UNRESOLVED generic method type parameter reaches the choice
// arm's own `equalsArgIsOwnTypeOrUnresolved` "or unresolved" clause
// (LlvmBackend_Calls2.cpp, `lowerValueSemMethodOn`'s choice branch), but THIS
// instantiation substitutes it to a NON-STRUCT type (`int`) rather than a
// choice — the pre-existing bare `if (!other->getType()->isStructTy())
// return nullptr;` there, present since #3893/#3538's rework and left
// untouched by #3913's adjacent struct-mismatch guard (filed as issue #3925,
// the gap #3922's own commit — the record twin — flagged as unfixed).
//
// MEASURED on HEAD (7cf24f7, #3922 already landed for the RECORD arm only):
// `bezelc` on this program compiles clean (rc=0) with only the pre-existing
// #2491/#3079 `Severity::Warning` "the argument of this `Console.WriteLine`
// failed to lower to a value" internal note (`hasErrors()` stays false).
// Running the produced binary prints `before` and `after` with NOTHING in
// between — the `s.Equals(arg)` comparison line vanishes completely, not
// even a `False` line — an unambiguous wrong answer (this shape is
// deliberately used instead of a `return`-shape oracle, which would
// coincidentally print the §8.3-correct `False` for an `int` against a
// choice and could not distinguish "correctly computed" from "defaulted to
// zero by accident" — see #3922's own commit for the identical reasoning).
//
// Post-fix, the arm reports a located `Severity::Error` naming the receiver
// type and rejects the build (`r.ran` false) instead of degrading the
// statement to nothing. This call site is a PLAIN `.` access (`s: Sh`, not
// `Sh?`) — the OTHER access path, `s?.Equals(arg)` on `s: Sh?`, is the
// DIFFERENT `?.` spelling pinned by
// `nullcond_unit_receiver_equals_hashcode_3452_test.cpp`'s T6 row. At the
// time this fix (#3925) landed, T6 was pinned to a DIFFERENT diagnostic (the
// `?.` wrapper's own generic §7.5 text, via a `viaNullCondWrapper` parameter
// this fix added so the arm stayed silent under `?.`); fix #3943 later
// reversed that choice — this arm's own text was found strictly more useful
// — and updated T6 (and added a T8) to assert THIS arm's text instead, so
// both access spellings now report the identical diagnostic below.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_equals_non_struct_generic_arg_fails_closed_not_degraded_3925) {
    auto r = runAot(
        "module M3893D; "
        "choice Sh Circle(R: int), Square(S: int); end Sh; "
        "class C "
        "  public static method Foo<Arg>(s: Sh, arg: Arg) "
        "    Console.WriteLine(\"before\"); "
        "    Console.WriteLine(s.Equals(arg).ToString()); "
        "    Console.WriteLine(\"after\"); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s := Sh.Circle(1); "
        "    C.Foo<int>(s, 7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("member `Equals`") != std::string::npos &&
            d.message.find("`M3893D.Sh`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
    bool explainsWhy = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not resolve to a value of that choice") !=
            std::string::npos)
            explainsWhy = true;
    BEZEL_EXPECT_TRUE(explainsWhy);
}

// ── T5 (fix #3925 false-decline control) — the SAME unresolved-parameter
// path, resolved to the choice's OWN type via `?.`, must keep answering a
// real value rather than newly declining. This is a narrower repeat of T5b
// in `nullcond_unit_receiver_equals_hashcode_3452_test.cpp` (which already
// covers `Foo<int>` on `Sh?`); pinned here too because it is the row this
// fix's `emittingGenericTemplateBody_` gating could most plausibly have
// broken by being too broad.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_equals_own_type_generic_arg_still_lowers_3925) {
    auto r = runAot(
        "module M3893E; "
        "choice Sh Circle(R: int), Square(S: int); end Sh; "
        "class C "
        "  public static method Foo<Arg>(s: Sh, arg: Arg): bool "
        "    return s.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s: Sh := Sh.Circle(1); "
        "    var other: Sh := Sh.Circle(1); "
        "    var diff: Sh := Sh.Square(1); "
        "    Console.WriteLine(\"same \" + C.Foo<Sh>(s, other).ToString()); "
        "    Console.WriteLine(\"diff \" + C.Foo<Sh>(s, diff).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("same True\ndiff False\n"));
}

// ── T6 (test #3954 — fix #3939's own repro, PLAIN `.` spelling, §8.1/§35.4/
// §35.7, v0.294) — the STRUCT-MISMATCH sibling of T4 above. #3939 closed a
// silent zero-degrade in `lowerValueSemMethodOn`'s choice arm: its
// struct-MISMATCH guard (`if (other->getType() != st)`, a few lines below
// T4's non-struct guard) let a PLAIN `.`/`!.` access on a NON-NULLABLE
// choice receiver, inside a generic method whose type parameter resolves at
// a live call site to a struct-shaped-but-MISMATCHED type (a record, here),
// compile clean and silently drop the comparison. #3939's own commit body
// gives this exact program as the MEASURED repro but only updated T6/T8 in
// nullcond_unit_receiver_equals_hashcode_3452_test.cpp — both the `?.`
// spelling reached through `lowerNullCondValueSemCall`'s wrapper — leaving
// the PLAIN `.` shape (the issue's own stated repro, and the shape most
// likely to regress independently of the `?.` wrapper, e.g. #3925/#3943
// already show the two paths can diverge) with no dedicated pin anywhere in
// the suite. `lowerValueSemMethodOn` is one shared function, so T8 does
// exercise the identical guard — this is a coverage gap, not a correctness
// one — but it is filled here rather than left implicit.
//
// FAIL-WITHOUT (parent 3092eb5, src/ reverted): rc=0, prints "False" —
// silently wrong, should have been rejected. PASS-WITH (post-#3939): rc=1,
// a located diagnostic naming `Equals` and `Sh`. A bare
// `BEZEL_EXPECT_FALSE(r.ran)` cannot tell "rejected at compile time" apart
// from a runtime crash or an ASan report (issue #3892) — this row uses
// `failedAtCompileTimeWithRealLocation` instead.
BEZEL_TEST(codegen_choice_equals_universal_argument_3893,
           choice_equals_struct_mismatch_generic_arg_fails_closed_plain_dot_3939) {
    auto r = runAot(
        "module M3939repro; "
        "choice Sh Circle(R: int), Square(S: int); end Sh; "
        "record Pair(A: int, B: int); "
        "class C "
        "  public static method Foo<X>(s: Sh, arg: X): bool "
        "    return s.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s: Sh := Sh.Circle(1); "
        "    var b := C.Foo<Pair>(s, new Pair(1, 2)); "
        "    Console.WriteLine(b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("member `Equals`") != std::string::npos &&
            d.message.find("`M3939repro.Sh`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
    bool explainsWhy = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not resolve to a value of that choice") !=
            std::string::npos)
            explainsWhy = true;
    BEZEL_EXPECT_TRUE(explainsWhy);
}

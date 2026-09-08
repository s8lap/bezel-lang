// fix #3452 — `?.Equals(…)` / `?.GetHashCode()` on a `()?` receiver emitted
// MALFORMED IR (spec §7.5 *Null-conditional access (`?.`)*, §8.1 *Definition*,
// §8.3 *`Equals` and `GetHashCode`*, §8.6 *Boxing*, §4.7 *The unit type* —
// v0.283). This is the `?.` spelling of the receiver #3422 fixed for `.`.
//
// MEASURED on ef339fa (`bezelc repro.bzl -o out`, exit 1):
//
//     Basic Block in function 'main' does not have terminator!
//     label %ncvs.end
//     repro.bzl:5:23: internal: the argument of this `Console.WriteLine` failed
//                     to lower to a value, so the whole statement emits nothing
//                     … — #3079
//     bezelc: codegen error: IR verification failed (malformed IR emitted…)
//
// `u?.ToString()` on the same receiver worked (it prints `()`), so the axis is
// the member, not `?.`.
//
// ── WHAT THE BUG WAS ─────────────────────────────────────────────────────────
//
// §7.5's lowering opens two blocks (`ncvs.do`, `ncvs.end`), branches on the
// receiver's hasValue tag, and lowers the member against the NARROWED inner
// value — for a `()?` receiver that inner type is `()` (§4.7: "Wrapping the unit
// type with `?` (yielding `()?`) … produces an optional with two values, `null`
// and `()`"). The value-semantics core it calls handles primitives, enums,
// records and choices and answered NULL for a unit receiver, because #3422's
// unit answer was written inline in the `.`-form dispatch and reachable from
// nowhere else. The caller then returned null with the builder still inside
// `ncvs.do` — leaving both blocks unterminated.
//
// So the defect was DOUBLE, and both halves are repaired: the unit answer is now
// one shared decision that both spellings reach, and a declining inner lowering
// now closes its blocks so that a decline degrades to the located `#3079` note
// instead of an internal-consistency failure that names no construct.
//
// The malformed IR was CAUGHT — `verifyModule` runs unconditionally on the AOT
// path and the build failed loudly. Nothing silently miscompiled.
//
// ── WHAT THE ANSWER IS ───────────────────────────────────────────────────────
//
// Exactly the `.` form's, lifted by §7.5 ("The result type is always nullable")
// — `bool?` and `int?`, null when the receiver is null. §8.3's invariant ("if
// `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`") is what makes SHARING
// mandatory rather than tidy: the boxed, unboxed and `?.` spellings of the same
// one value must not disagree. T3 asserts that equality across all three rather
// than a magic number, so the row states §8.3's requirement and not the
// implementation's choice of constant.
//
// ── WHAT MAKES EACH ROW ABLE TO FAIL ─────────────────────────────────────────
//
// Pre-fix the compile FAILED, so every row fails at `r.ran`. Post-fix, a fix
// that answered a constant `True` passes T1 and dies on T2/T4; one that answered
// `0` for the hash dies on T3's non-zero and agreement checks; one that lifted
// the null receiver wrongly dies on T2; and one that evaluated the argument
// unconditionally dies on T5, which pins §7.5's short-circuit with a marker.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr const char* kDecls =
    "module M3452; "
    "class C "
    "  public static method Side(): () "
    "    Console.WriteLine(\"side\"); "
    "  end Side; ";

// A `bool?`/`int?` is printed through an explicit null test so the ROW's
// expected text distinguishes "null" from every non-null answer. (§20's
// conditional expression: the parentheses and the `else` are both required.)
std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ── T1 — THE ISSUE'S EXACT REPRO, on a PRESENT `()?`. Pre-fix `bezelc` exited 1
// with "IR verification failed"; post-fix both statements lower and print the
// §7.5-lifted answers.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_unit_equals_and_hashcode_lower_spec_7_5_8_3) {
    auto r = runAot(prog(
        "    var u: ()? := (); "
        "    var e := u?.Equals(()); "
        "    var h := u?.GetHashCode(); "
        "    Console.WriteLine((if e = null then \"null\" else e!.ToString())); "
        "    Console.WriteLine((if h = null then \"null\" else (h! <> 0).ToString())); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n"));
}

// ── T2 — THE NULL RECEIVER. §7.5: "`x?.M` returns null if `x` is null". Without
// this row a fix that answered the unit's constants unconditionally — ignoring
// the tag it was handed — passes T1 and is wrong for every null receiver.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_unit_null_receiver_answers_null_spec_7_5) {
    auto r = runAot(prog(
        "    var n: ()? := null; "
        "    var e := n?.Equals(()); "
        "    var h := n?.GetHashCode(); "
        "    var s := n?.ToString(); "
        "    Console.WriteLine((if e = null then \"null\" else e!.ToString())); "
        "    Console.WriteLine((if h = null then \"null\" else h!.ToString())); "
        "    Console.WriteLine(s ?? \"null\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null\nnull\nnull\n"));
}

// ── T3 — §8.3's INVARIANT across all THREE spellings of the one unit value.
// `u.Equals(o)` is true for a boxed `()`, so §8.3 requires the hashes to agree;
// the `?.` answer is therefore not free to be picked independently. Also pins
// non-zero, because `0` is what every dropped / zero-initialised path produces
// (#2804's trap) and a row that accepted it would pass vacuously.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_boxed_and_unboxed_unit_hashes_agree_spec_8_3_8_6) {
    auto r = runAot(prog(
        "    var p: ()? := (); "
        "    var u: () := (); "
        "    var o: Object := (); "
        "    var hp := p?.GetHashCode(); "
        "    var hu := u.GetHashCode(); "
        "    var ho := o.GetHashCode(); "
        "    Console.WriteLine((if hp = null then \"null\" else (hp! = hu).ToString())); "
        "    Console.WriteLine((if hp = null then \"null\" else (hp! = ho).ToString())); "
        "    Console.WriteLine((hu <> 0).ToString()); "
        "    Console.WriteLine((if hp = null then \"null\" else (hp! = p?.GetHashCode()!).ToString())); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nTrue\nTrue\n"));
}

// ── T4 — the ARGUMENT axis, both directions. §8.6's nullable rule ("If the
// value is non-null, the result is a boxed `T` … The nullable wrapper is
// stripped during boxing", and a null one becomes "a null `Object` reference")
// is what makes a PRESENT `()?` argument equal and a NULL one not. The three
// FALSE rows are what stops a fix that answers `True` unconditionally.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_unit_equals_classifies_its_argument_spec_8_3_8_6) {
    auto r = runAot(prog(
        "    var u: ()? := (); "
        "    var v: () := (); "
        "    var p: ()? := (); "
        "    var z: ()? := null; "
        "    var o: Object := (); "
        "    var oi: Object := 7; "
        "    Console.WriteLine(u?.Equals(())!.ToString()); "
        "    Console.WriteLine(u?.Equals(v)!.ToString()); "
        "    Console.WriteLine(u?.Equals(p)!.ToString()); "
        "    Console.WriteLine(u?.Equals(o)!.ToString()); "
        "    Console.WriteLine(u?.Equals(z)!.ToString()); "
        "    Console.WriteLine(u?.Equals(oi)!.ToString()); "
        "    Console.WriteLine(u?.Equals(5)!.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nTrue\nTrue\nTrue\nFalse\nFalse\nFalse\n"));
}

// ── T5 — §7.5's SHORT-CIRCUIT, which the shared answer must not break. "The
// receiver is evaluated first; if it is null the access does not occur and
// nothing to its right in the chain is evaluated — no member is read, no call is
// performed, and the argument … expressions of the skipped suffixes are not
// evaluated either." The `.` form's arm lowers the argument unconditionally
// (§22.5), so routing `?.` through the same answer is only correct if the
// lowering lands INSIDE the non-null arm. The markers pin both directions: no
// `side` for the null receiver, exactly one for the non-null one.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_unit_equals_argument_short_circuits_spec_7_5) {
    auto r = runAot(prog(
        "    var n: ()? := null; "
        "    var u: ()? := (); "
        "    Console.WriteLine(\"A\"); "
        "    var a := n?.Equals(C.Side()); "
        "    Console.WriteLine((if a = null then \"null\" else a!.ToString())); "
        "    Console.WriteLine(\"B\"); "
        "    var b := u?.Equals(C.Side()); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("A\nnull\nB\nside\nTrue\n"));
}

// ── T5b (fix-forward #3896 gate recovery, spec §8.1/§8.3/§35.7, v0.294) —
// the CHOICE arm's #3893 fix, exercised through THIS file's `?.` spelling.
// §8.1 gives every value an `Equals(other: Object?)`; an `int` argument's
// static type can never BE a `Sh` value, so it settles False WITHOUT ever
// boxing the receiver (fix #3893's `equalsArgSettlesFalse`, shared with the
// record/enum arms — LlvmBackend_Calls2.cpp). MEASURED: pre-#3893 `bezelc`
// declined this exact program (it is the program the OLD T6 row below used
// to pin, before its receiver moved a second time — see the comment there
// for why); on this tree it compiles clean and prints `False`.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_choice_equals_settles_false_against_primitive_arg_spec_8_1_8_3) {
    auto r = runAot(
        "module M3452F; "
        "choice Sh "
        "  Circle(R: int), "
        "  Square(S: int); "
        "end Sh; "
        "class C "
        "  public static method Main(): int "
        "    var s: Sh? := Sh.Circle(1); "
        "    var b := s?.Equals(7); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// ── T6 — HALF (b) MUST FAIL CLOSED. This is the row the first version of this
// fix did not have, and its absence let a blocker through review.
//
// Closing `ncvs.do`/`ncvs.end` makes the IR well-formed, but on its own it turns
// a compile ERROR into a compiling program with a WRONG VALUE: the caller's
// fallback for a null result is `#3079`/`#2434`, both `Severity::Warning`, so the
// build succeeds and the `?.` result comes back all-zero — i.e. `null`. §7.5
// reserves that answer for a null receiver ("`x?.M` returns null if `x` is
// null"), so a NON-NULL receiver answering `null` is unreadable by construction.
//
// MEASURED, on the RECORD receiver this row originally used
// (`p?.Equals(true)` with `var p: Pt? := new Pt(1, 2)`):
//   base (pre-#3452)          rc=1, "Basic Block ... does not have terminator!"
//   block-closing only        rc=0, prints `A null`   <-- the fail-open
//   #3452's follow-up         rc=1, the located error asserted below
//
// ⚠ THE RECEIVER MOVED, AND THE ROW DID NOT WEAKEN (fix #3538). That record
// decline was `lowerValueSemMethodOn`'s record arm refusing an `Equals`
// argument that is not itself a record — and it was a DEFECT, not a rule:
// §8.1 gives every value an `Equals(other: Object?)` and §8.6 boxes the
// argument, so `p?.Equals(true)` is `False`, which is what it now answers
// (measured: rc=0, prints `False`). Keeping this row on that program would
// have pinned the defect, so it moved to a receiver whose decline #3538
// deliberately did NOT remove: a CHOICE, whose arm at the time still refused
// a value-primitive argument — issue 3545, with the measured table.
//
// ⚠⚠ THE RECEIVER MOVED A SECOND TIME (fix-forward #3896 gate recovery,
// spec §8.1/§8.3/§35.7/§35.8, v0.294), FOR THE SAME REASON: the CHOICE
// decline the paragraph above relied on was ITSELF a defect, and #3893
// (gate the choice `Equals` arm on the static TypeRef, not a bare
// `isStructTy()`) closed it, with #3900 (box a value-like/borrow-bearing
// choice) closing the sibling boxing gap in the same batch. `Sh` is
// VALUE-LIKE (§35.4: both cases hold a plain `int` payload, no `owned`, no
// borrow) — never owning-classified — so "§8.6 forbids boxing an
// owning-classified choice" never actually applied to THIS receiver; #3893
// simply hadn't yet routed the choice arm's `Equals` through the same
// `equalsArgSettlesFalse` the record/enum arms already used. MEASURED on
// this tree: `s?.Equals(7)` on `Sh` now compiles and prints `False`
// (rc=0) — T5b above pins that VALUE. Keeping this row on `Sh.Equals(7)`
// would pin the now-fixed defect, exactly as keeping it on `Pt.Equals(true)`
// would have after #3538 — so it moves again, per the same "the row did
// not weaken" discipline: every assertion below is unchanged in kind; only
// the receiver/argument SHAPE differs.
//
// ⚠⚠⚠ THE ASSERTED TEXT MOVED A THIRD TIME (fix #3943, §8.1/§9.1/§9.2/§35.4/
// §35.7, v0.294) — the row did NOT weaken, only the SOURCE of the diagnostic
// changed. Every prior version of this row asserted THIS function's own
// `?.`-wrapper text (`lowerNullCondValueSemCall`'s "declining inner lowering
// fails closed", citing bare §7.5) — the choice arm's own non-struct decline
// (`lowerValueSemMethodOn`, LlvmBackend_Calls2.cpp) deliberately stayed
// silent under `?.` via a `viaNullCondWrapper` parameter #3925 added
// specifically to keep this row's text unchanged. #3943 reversed that after
// a review compared the two diagnostics side by side: the wrapper's text
// above is generic boilerplate restating §7.5, while the arm's own text
// names the receiver's type, cites §35.7's structural-equality rule
// directly, and gives two concrete remedies (retype the argument, or compare
// through `Object`) — strictly more useful for identical information. The
// arm now reports unconditionally regardless of how it is reached, and
// returns a non-null `PoisonValue`, so this function's own `if (!res)` check
// never sees a null for this path any more and never fires its own
// diagnostic here — the WRAPPER'S text asserted by every prior version of
// this row no longer appears for this program at all. What this row proves
// is unchanged in kind (the compile is REJECTED, with a diagnostic naming
// `Equals` and `M3452F.Sh`) — only which of the two pre-existing diagnostic
// texts is the one produced.
//
// ⚠⚠⚠⚠ THE ROW REVERSES ONE FINAL TIME (fix #3914, §8.1/§8.3/§8.6/§35.4/§35.7,
// v0.294). The residual decline this row pinned — an UNRESOLVED generic method
// type parameter (`X` in `Foo<X>(s: Sh?, arg: X)`) routed through
// `equalsArgIsOwnTypeOrUnresolved`'s "or unresolved" clause into the
// structural-equality path, which then declined on `!isStructTy()` when `X`
// resolved to `int` at the live call site — was the spec gap #3914 catalogued:
// §8.1's universal `Equals(other: Object?)` and §8.6's boxing guarantee a
// boxed `int` can never equal a boxed `Sh`, so the program below asks the
// IDENTICAL runtime question as the concrete `s?.Equals(7)` already pinned by
// T5b above and the answer must be the SAME: `False`. Resolving `at` through
// `resolveTypeParamRef` BEFORE running the predicates (LlvmBackend_Calls2.cpp's
// choice/record/enum arms, all three) routes `Foo<int>(s, 7)` to
// `equalsArgSettlesFalse` directly and emits `False` per §8.3/§8.6 —
// matching T5b. The dead abstract-template pass is unaffected (its
// `genericSubstSemaTy_` is empty, so `resolveTypeParamRef` returns the bare-
// unresolved ref unchanged). What this row now proves is: a generic method
// whose type parameter resolves to a known-settles-False argument compiles,
// runs, and PRINTS `False` (rc=0) — strictly better than the prior
// diagnostic-only outcome and identical in kind to T5b's concrete spelling.
// (The row's name and the `spec_7_5` test-suite suffix are unchanged so the
// test's identity carries through; the assertion below is updated for the
// new, more-correct `False` answer per LANE_RULES §7: a fix reversing a
// prior rejection MUST update the test it displaced.)
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_declining_inner_lowering_fails_closed_spec_7_5) {
    auto r = runAot(
        "module M3452F; "
        "choice Sh "
        "  Circle(R: int), "
        "  Square(S: int); "
        "end Sh; "
        "class C "
        "  public static method Foo<X>(s: Sh?, arg: X): bool? "
        "    return s?.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s: Sh? := Sh.Circle(1); "
        "    var b := C.Foo<int>(s, 7); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3914: the program now COMPILES and prints `False` per §8.3/§8.6 —
    // matching the concrete `s?.Equals(7)` spelling T5b above already pins —
    // rather than declining. The dead abstract-template pass may still emit a
    // benign #2491 "return-value expression failed to lower" `Severity::Warning`
    // backstop for the i32-placeholder dead body (an unrelated pre-existing
    // quirk of the dead-template lowerReturnStmt path; the live
    // `Foo<int>` specialization lowers correctly and is what runs).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// ── T7 — BOTH-SIDES CONTROL. Every other row in this file fails 100% without
// the fix, which is right for a crash/reject fix but leaves nothing that would
// catch a harness break, a syntax slip in the shared `prog()` scaffold, or a
// stdlib change — a file that is all-red on one side and all-green on the other
// cannot distinguish "the fix works" from "the file stopped building". This row
// uses an `int?` receiver, touches no unit-type code path, and passes on BOTH
// sides of the fix by construction.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           control_nullcond_on_int_receiver_unaffected_spec_7_5_8_3) {
    auto r = runAot(prog(
        "    var p: int? := 5; "
        "    var n: int? := null; "
        "    var e := p?.Equals(5); "
        "    var f := p?.Equals(6); "
        "    var g := n?.Equals(5); "
        "    Console.WriteLine((if e = null then \"null\" else e!.ToString())); "
        "    Console.WriteLine((if f = null then \"null\" else f!.ToString())); "
        "    Console.WriteLine((if g = null then \"null\" else g!.ToString())); "
        "    var h := p?.GetHashCode(); "
        "    Console.WriteLine((if h = null then \"null\" else (h! = 5).ToString())); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nnull\nTrue\n"));
}

// ── T8 (fix #3913, §8.1/§8.3/§35.4/§7.5, v0.294) — the SIGSEGV sibling of T6's
// decline. T6's `Foo<int>(s, 7)` reaches the choice `Equals` arm's unresolved-
// type-parameter path and declines cleanly because `X` substitutes to `int`
// (not a struct, so the arm's own `isStructTy()` gate already caught it, pre-
// #3913). This row substitutes `X` at a STRUCT-shaped type instead
// (`Pair`, an unrelated 2-field record) — `equalsArgIsOwnTypeOrUnresolved`
// (LlvmBackend_impl.hpp) still answers TRUE for the unresolved `X` (it cannot
// see the substitution), so pre-fix codegen fed `Pair`'s loaded struct value
// straight into `emitChoiceEquality`, which unconditionally extracts fields at
// `Sh`'s OWN choice-struct indices — a mismatched-type `CreateExtractValue`
// that SIGSEGVs `bezelc` itself (MEASURED via gdb: fault in
// `llvm::Value::setName`, called from `ExtractValueInst::Create` inside
// `emitChoiceEquality`, LlvmBackend_Layout.cpp, reached through
// `lowerValueSemMethodOn`'s choice arm, LlvmBackend_Calls2.cpp). Post-fix, the
// mismatched-shape guard added for #3913 fails this CLOSED exactly like T6 —
// same diagnostic, same "declining inner lowering" path — rather than
// crashing. A bare not-ran check on its own is deliberately not the oracle
// here (it is also satisfied by a crash or a signal death, per LANE_RULES
// §3): the diagnostic TEXT is asserted below, which only the fixed-closed
// outcome produces — the base tree's SIGSEGV asserts nothing here at all
// (whether the run "ran" is undefined after a child crash, not merely
// false), which is exactly the FAIL-WITHOUT asymmetry this row exists to
// prove: pre-fix, `bezelc` (and the harness's forked child running it) dies
// by signal instead of returning a diagnostic.
//
// ⚠ THE ASSERTED TEXT MOVED (fix #3939 folded into fix #3943, §8.1/§35.4/
// §35.7, v0.294), for the SAME reason and at the SAME time as T6's: #3913
// left this exact decline a BARE `return nullptr;` with no diagnostic of its
// own, so pre-#3939 this row's fail-closed behavior came ENTIRELY from the
// `?.` wrapper's generic text (the same one T6 used to assert). #3939 gave
// the choice arm's struct-mismatch guard its own located diagnostic — IDENTICAL
// text to the non-struct guard's (T6), mirroring the record arm's own
// established precedent of using one message for both its non-struct and
// struct-mismatch guards (`record_enum_equals_universal_argument_3538_test.cpp`'s
// T7/T9) — so this row asserts the SAME "does not resolve to a value of that
// choice" text T6 now does, not a distinct one.
//
// ⚠⚠ THE ROW REVERSES (fix #3914, §8.1/§8.3/§8.6/§35.4/§35.7, v0.294) — for
// the SAME reason T6 above reverses. `at = resolveTypeParamRef(X)` substitutes
// to `Pair` (a record) at this call site; `equalsArgSettlesFalse(Pair, Sh)`
// returns TRUE (different record type per §8.3) and emits `False` directly.
// `Pair` is still a struct — #3913's mismatch guard remains in place for
// the dead abstract-template pass and any genuinely-unresolved case where
// `resolveTypeParamRef` returns `ty` unchanged — but the LIVE `Foo<Pair>`
// specialization never reaches it. The row name and `not_crash_3913` suffix
// stay (the crash fix itself is not undone); the assertion below is updated
// for the new `False` answer.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_choice_struct_shaped_generic_arg_fails_closed_not_crash_3913) {
    auto r = runAot(
        "module M3452F; "
        "choice Sh "
        "  Circle(R: int), "
        "  Square(S: int); "
        "end Sh; "
        "record Pair(A: int, B: int); "
        "class C "
        "  public static method Foo<X>(s: Sh?, arg: X): bool? "
        "    return s?.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s: Sh? := Sh.Circle(1); "
        "    var b := C.Foo<Pair>(s, new Pair(1, 2)); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3914: the program now COMPILES and prints `False` per §8.3/§8.6 —
    // `Pair` is a record, `Sh` is a choice, §8.3 settles the answer at `False`
    // — matching the concrete spelling of the same runtime question.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// ── T9 (fix #3942, §7.5/#3452, v0.294) — HIGH-SEVERITY FALSE-REJECT CONTROL.
// `lowerNullCondValueSemCall`'s own decline-fails-closed diagnostic (T6/T8's
// wrapper text) had no `emittingGenericTemplateBody_` guard, unlike every
// arm-level decline diagnostic in `lowerValueSemMethodOn`. A generic method's
// body is compiled TWICE: once as a dead, never-invoked "abstract template"
// pass (where an unresolved type parameter's placeholder is always `i32`),
// and once per live instantiation. `Foo<X>(s: Sh?, arg: X)` below is called
// ONLY as `Foo<Sh>(s, other)` — `X` substitutes to the RECEIVER'S OWN type, a
// completely well-typed instantiation that should compute the real §35.7
// answer — but pre-fix, the dead pass's harmless `i32`-placeholder decline
// was unconditionally promoted to a hard compile error, rejecting this
// program independent of what its only live call site instantiated `X` to.
// MEASURED on the pre-#3942 tree: `bezelc` on this exact program exited 1
// with T6's old wrapper diagnostic; the plain `.` spelling of the identical
// program (`s: Sh`, not `Sh?`) was unaffected (compiled clean and printed
// `True`), confirming the defect was specific to the `?.` wrapper, not the
// dispatch core's classification of the call.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_choice_own_type_generic_arg_via_null_cond_still_lowers_3942) {
    auto r = runAot(
        "module M3942A; "
        "choice Sh "
        "  Circle(R: int), "
        "  Square(S: int); "
        "end Sh; "
        "class C "
        "  public static method Foo<X>(s: Sh?, arg: X): bool? "
        "    return s?.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var s: Sh? := Sh.Circle(1); "
        "    var other := Sh.Circle(1); "
        "    var diff := Sh.Square(1); "
        "    var same := C.Foo<Sh>(s, other); "
        "    var notSame := C.Foo<Sh>(s, diff); "
        "    Console.WriteLine((if same = null then \"null\" else same!.ToString())); "
        "    Console.WriteLine((if notSame = null then \"null\" else notSame!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// ── T10 (fix #3942, §7.5/#3452, v0.294) — the RECORD twin of T9. Issue #3942's
// own repro used a record receiver to show the defect is a property of the
// shared `?.` wrapper, not specific to the choice arm's classification logic;
// this row pins that repro directly (`p: Pt?`, `Foo<Pt>(p, other)`).
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_record_own_type_generic_arg_via_null_cond_still_lowers_3942) {
    auto r = runAot(
        "module M3942B; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Foo<Arg>(p: Pt?, arg: Arg): bool? "
        "    return p?.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var p: Pt? := new Pt(1, 2); "
        "    var same: Pt := new Pt(1, 2); "
        "    var diff: Pt := new Pt(9, 9); "
        "    var a := C.Foo<Pt>(p, same); "
        "    var b := C.Foo<Pt>(p, diff); "
        "    Console.WriteLine((if a = null then \"null\" else a!.ToString())); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// ── T11 (fix #3943, §8.1/§9.1/§9.2/§35.4/§35.7, v0.294) — the RECORD twin of
// T6/T8's single-diagnostic assertion, pinning issue #3943's own repro
// directly: a nullable record accessed via `?.` with a generic method's
// instantiation genuinely NOT resolving to that record (`Foo<int>(p, 7)`)
// must produce EXACTLY ONE diagnostic for the one call site. MEASURED on the
// pre-#3942/#3943 tree: this exact program printed BOTH the `?.` wrapper's
// generic text (from the dead template pass, #3942's bug) AND the record
// arm's own specific text (from the live pass, #3922's unconditional report)
// — two diagnostics for one call site.
//
// ⚠ THE ROW REVERSES (fix #3914, §8.1/§8.3/§8.6/§9.1/§9.2, v0.294). The
// resolve-first change at the record arm routes `Foo<int>(p, 7)` through
// `equalsArgSettlesFalse(int, Pt)` — `int` is a primitive, `Pt` is a record,
// §8.3 settles the answer at `False` — and emits `False` directly from the
// dispatch core. The double-diagnostic defect this row originally pinned is
// now structurally impossible: the live pass settles `False` per §8.3
// without reaching the diagnostic path at all, and the dead abstract-template
// pass still degrades silently under the existing
// `emittingGenericTemplateBody_` gates #3922/#3942 added. The row name and
// `single_diagnostic_not_double_3943` suffix are kept (the double-diagnostic
// fix itself is not undone — it remains load-bearing for any genuinely-
// unresolved path that the resolve-first cannot subsume), but the assertion
// below is updated for the new, more-correct `False` answer per LANE_RULES
// §7.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_record_bad_instantiation_single_diagnostic_not_double_3943) {
    auto r = runAot(
        "module M3943; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Foo<Arg>(p: Pt?, arg: Arg): bool? "
        "    return p?.Equals(arg); "
        "  end Foo; "
        "  public static method Main(): int "
        "    var p: Pt? := new Pt(1, 2); "
        "    var b := C.Foo<int>(p, 7); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3914: the program now COMPILES and prints `False` per §8.3/§8.6 —
    // `int` against `Pt?` settles to `False` per §8.3 with §8.6 boxing
    // (a boxed int cannot equal a record).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

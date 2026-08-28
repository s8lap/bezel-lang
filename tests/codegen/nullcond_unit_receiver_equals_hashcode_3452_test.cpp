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
// MEASURED, `p?.Equals(true)` on `var p: Pt? := new Pt(1, 2)`:
//   base (pre-#3452)          rc=1, "Basic Block ... does not have terminator!"
//   block-closing only        rc=0, prints `A null`   <-- the fail-open
//   this commit               rc=1, the located error asserted below
//
// The live decline behind it is #3538 (`lowerValueSemMethodOn`'s record arm
// declines when the `Equals` argument is not itself a record — the sibling the
// `Object`-receiver path got `lowerUniversalEqualsArg` for in #3443). That is
// owned elsewhere; this row's job is to pin that the decline stays fail-CLOSED
// and cannot regress into `null` again.
//
// The TEXT is asserted, not `hasErrors()` (which the IR-verifier abort also
// satisfies) and not `!ran` alone (which the BASE also satisfies) — only the new
// diagnostic distinguishes this commit from both other states.
BEZEL_TEST(codegen_nullcond_unit_receiver_equals_hashcode_3452,
           nullcond_declining_inner_lowering_fails_closed_spec_7_5) {
    auto r = runAot(
        "module M3452F; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt? := new Pt(1, 2); "
        "    var b := p?.Equals(true); "
        "    Console.WriteLine((if b = null then \"null\" else b!.ToString())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool located = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "reserves the `null` result of `?.` for a null receiver") !=
            std::string::npos)
            located = true;
    BEZEL_EXPECT_TRUE(located);
    // …and it must name the member and the receiver type, which is the whole
    // point of replacing "IR verification failed" with a real diagnostic.
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("member `Equals`") != std::string::npos &&
            d.message.find("`M3452F.Pt`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
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

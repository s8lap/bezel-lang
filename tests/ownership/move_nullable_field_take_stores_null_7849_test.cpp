// Regression tests for issue #7849 — a bare take of a NULLABLE owned field
// must NOT report "field `X` left in moved-from state at method exit".
//
// §28.8.1 (*The replacing `move`*, v0.336 — the revision that replaced the
// restoration window; the current text is v0.355):
//
//   "Where the location's declared type is **nullable** — read as declared:
//    `T?`, a nullable slot type, and not a bare type parameter `T`, whatever its
//    substitution — the `with` clause may be omitted: `move x.f` stores `null`."
//
//   "**No location ever holds a hole** ([K9.2]). ... the *moved-from* state is a
//    state of bindings only ([K1]). Nothing therefore needs restoring after a
//    `move`, no exit path carries an obligation ..."
//
//   "A nullable location a bare take leaves holding `null` is an ordinary null
//    field: its holder's cascade disposes nothing through it, and a later read
//    reads `null`."
//
// ⇒ A nullable owned field's bare take is the replacing `move`: it stores `null`.
// The field is left holding `null` — never a hole, never a moved-from state — and
// owes NO restoration. The ownership pass nevertheless modelled the retired
// restoration window (`fieldMovedFrom_`), so the at-method-exit report fired on
// the issue's own repro (the conformance case `s28_8_1_0018`, a PASS row).
//
// FAIL-WITHOUT / PASS-WITH. The raise row below (a bare take of a nullable owned
// field) is REJECTED on the base (a30bb11d) by the retired restoration window
// and compiles clean with the fix; its runtime twin runs under the checked AOT
// oracle, which is where the take's `null` store is observed (the field reads
// null afterwards) and where the exactly-once disposal is observed (the holder's
// cascade disposes nothing through the nulled field, the taker disposes the
// value once).
//
// The twin rows pin the matrix around the fix: a NON-nullable field's bare take
// is still rejected (the hole it would leave is real — the `with` clause that
// §28.8.1 requires there is not implemented yet, tracker #5923), a moved-from
// LOCAL is still rejected (§28.3: the moved-from state is a state of bindings),
// and a field that is re-assigned before the exit is still accepted.
//
// NOTE on scope. #5923 (the replacing `move` proposal 0174) owns the FULL
// retirement of the restoration window; it cannot land before `move loc with e`
// is implemented, because a non-nullable location's bare take has no other
// legal spelling until then. This file covers the NULLABLE slice only.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res: ctor ++, dtor --. A correct program leaves
// live == 0; a double-dispose drives it negative, and the checked AOT oracle
// (ASan + LSan) catches the double-free itself.
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public constructor() Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

// The issue's repro shape, as a source string: a holder with a NULLABLE owned
// field, a take method that bare-takes it, and a property that reads the field
// back. `IsEmpty` is the observable: after the take the field holds `null`, so
// the read is True.
const char* kNullableHolder =
    "class Holder "
    "  private owned _c: Res? := new Res(); "
    "  public method Detach(): owned Res return move _c!; end Detach; "
    "  public property IsEmpty: bool => _c = null; "
    "end Holder; ";

}  // namespace

// ===========================================================================
// THE RAISE ROW — the issue's repro: a bare take of a NULLABLE owned field.
// ===========================================================================

// §28.8.1 (v0.336): the take stores `null`; the field is never moved-from, so
// the method owes no restoration and the retired at-exit report must not fire.
// FAILS on the base (a30bb11d) — the at-exit report rejects this program.
BEZEL_TEST(move_nullable_field_take_7849,
           nullable_field_bare_take_is_not_reported_at_exit_spec_28_8_1) {
    auto r = parseAndCheckOwnership(std::string(kRes) + kNullableHolder +
                                    "class C public static method Main(): int "
                                    "  using var h := new Holder(); "
                                    "  using var t := h.Detach(); "
                                    "  return 0; "
                                    "end Main; "
                                    "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The retired diagnostic is named explicitly, so a DIFFERENT rule rejecting
    // this program cannot make the row pass (the #2988 "which rule fired"
    // discipline).
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "left in moved-from state"));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "already moved-from"));
}

// The runtime twin: the take stores `null`, the holder's cascade disposes
// nothing through the nulled field, and the taker disposes the value exactly
// once. `IsEmpty` is printed because the exit code is 8-bit truncated (the
// lane-contract's item 9) — the field's post-take state is the observable.
// The live count is read AFTER `Take()`'s scope has ended (the `using var`s
// dispose at that scope's end), so `Res.live == 0` is the exactly-once oracle.
BEZEL_TEST(move_nullable_field_take_7849,
           nullable_field_bare_take_stores_null_and_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kRes) + kNullableHolder +
        "class C "
        "  public static method Main(): int "
        "    Take(); "
        "    return Res.live; "
        "  end Main; "
        "  public static method Take(): int "
        "    using var h := new Holder(); "
        "    using var t := h.Detach(); "
        "    Console.WriteLine($\"{h.IsEmpty}\"); "
        "    return 0; "
        "  end Take; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // Res.live == 0: disposed exactly once
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

// ===========================================================================
// The matrix around the fix (the 19a sweep, pinned).
// ===========================================================================

// The NON-nullable twin: a bare take of a NON-nullable owned field still leaves
// the field EMPTY, so it is still rejected. §28.8.1 requires the `with` clause
// at a non-nullable location; the compiler does not implement `move loc with e`
// yet (tracker #5923), so the at-exit report is what stands in for it.
// GREEN on the base and on the fix — the raise row's admitting control.
BEZEL_TEST(move_nullable_field_take_7849,
           nonnullable_field_bare_take_is_still_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder "
        "  private owned _c: Res; "
        "  public constructor() _c := new Res(); end constructor; "
        "  public method Detach(): owned Res return move _c; end Detach; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A moved-from LOCAL is still unreadable until it is reassigned (§28.3) — the
// moved-from state is a state of BINDINGS, and §28.8.1's retirement of the
// field's window says nothing about the binding's.
BEZEL_TEST(move_nullable_field_take_7849,
           moved_from_local_is_still_unreadable_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class C public static method Main(): int "
        "  var r := new Res(); "
        "  var s := move r; "
        "  s.Dispose(); "
        "  r.Dispose(); "
        "  return 0; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "was already moved out at this point"));
}

// A field that is re-assigned before the exit is still accepted — the
// restoration window's positive control, green on the base and on the fix.
BEZEL_TEST(move_nullable_field_take_7849,
           field_move_then_restore_is_still_accepted_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder "
        "  private owned _c: Res; "
        "  public constructor() _c := new Res(); end constructor; "
        "  public method Swap(): owned Res "
        "    var d := move _c; "
        "    _c := new Res(); "
        "    return d; "
        "  end Swap; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A NULLABLE field taken bare on a path that restores it only CONDITIONALLY is
// accepted: the take stores `null`, so there is nothing to restore on either
// path, and the field's post-take state is `null` either way.
BEZEL_TEST(move_nullable_field_take_7849,
           nullable_field_conditional_restore_is_accepted_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder "
        "  private owned _c: Res? := new Res(); "
        "  public method Maybe(flag: bool): owned Res? "
        "    if flag then _c := null; end if; "
        "    return move _c; "
        "  end Maybe; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A second bare take of the SAME nullable field is legal: the first take left
// the field holding `null`, so the second take yields `null` — there is no
// ownership left to transfer and no double-own. The retired "already moved-from"
// report must not fire.
BEZEL_TEST(move_nullable_field_take_7849,
           second_bare_take_of_a_nullable_field_is_accepted_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder "
        "  private owned _c: Res? := new Res(); "
        "  public method Twice(): bool "
        "    var a := move _c; "
        "    var b := move _c; "
        "    a?.Dispose(); "
        "    b?.Dispose(); "
        "    return b = null; "
        "  end Twice; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "already moved-from"));
}

// ===========================================================================
// The nullability is read AS DECLARED, not as substituted (§28.8.1).
// ===========================================================================

// §28.8.1: the nullability is read "as declared: `T?`, a nullable slot type,
// and **not a bare type parameter `T`, whatever its substitution**". A field
// declared at a bare `T` is therefore NOT a nullable location, even when the
// instantiation substitutes a nullable type for `T` — so the bare take is
// still rejected there.
BEZEL_TEST(move_nullable_field_take_7849,
           bare_type_parameter_field_is_not_a_nullable_location_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder<T> "
        "  private owned _c: T; "
        "  public constructor(c: owned T) _c := c; end constructor; "
        "  public method Detach(): owned T return move _c; end Detach; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Res?>(new Res()); "
        "  return 0; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same generic class with the field declared `T?` IS a nullable location
// (the declared `T?` is a nullable type), so the bare take stores `null` and
// is accepted.
BEZEL_TEST(move_nullable_field_take_7849,
           nullable_type_parameter_field_is_a_nullable_location_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kRes) +
        "class Holder<T> "
        "  private owned _c: T?; "
        "  public constructor(c: owned T) _c := c; end constructor; "
        "  public method Detach(): owned T? return move _c; end Detach; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Res>(new Res()); "
        "  return 0; "
        "end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "left in moved-from state"));
}

// Spec-derived regression test for issue #4344 — §9.7 / §28.3 (v0.298):
// record `with(name := expr, …)` non-destructive-update value bound to a
// borrow-carrying field must NOT admit a temporary whose lifetime ends at
// the `with(...)` expression (the borrow-lifetime rule the positional
// `new Record(<temporary>)` twin already enforces via
// `MethodOwnership::checkRecordFromTemporary`).
//
// BACKGROUND (traced, `src/ownership/Ownership_Checks.cpp`).
//
// `checkRecordFromTemporary` walks `NewExpr::args` positionally against
// `recordPositionalFieldSymbols(recTs)`. `WithExpr::assigns` is a
// NAME-keyed `InitAssign{name, value}` list, so the same positional logic
// cannot be reused as-is — a `with`-specific twin that matches each
// `InitAssign.name` to the source record's declared member
// (`recordPositionalFieldSymbols` and the broader `members` walk both
// surface the right symbol) is needed.
//
// narrow #4330 (`tests/sema/with_expr_collection_literal_element_interface
// _conformance_4330_test.cpp`) closed the §5.1/§11.2
// interface-conformance half of the with-clause gap at the sema pass;
// the §9.7/§28.3 borrow-lifetime half was DEFERRED to this issue because
// the fix needs to touch `src/ownership/`, which that lane did not own.
//
// Each test below pins one direction. The REJECT side asserts on
// diagnostic TEXT (the §9.7 message), not the coarse `rejects()` — a
// runAot SIGSEGV path synthesizes a generic Error diagnostic that would
// otherwise mask the real under-reject (LANE_RULES §3).
//
// per §9.7 (v0.298), §28.3 (v0.298), #4344.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic whose message contains `needle` was emitted.
// Mirrors named_arg_borrow_bearing_field_walker_2268_test.cpp's hasError
// pattern.
bool hasError(const std::string& src, const std::string& needle) {
    auto r = parseAndCheckOwnership(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// DIRECTION 1 — UNDER REJECT (the bug).
//
// §9.7 / §28.3: a record `Box(Items: Array<IHolder>, N: int)` whose
// `Items` field is borrow-carrying (a heap-allocated `Array<IHolder>` is
// a builtin generic with no user TypeDecl, classified borrow-carrying by
// `MethodOwnership::fieldTypeIsBorrowCarrying`).
//
// A `with(Items := new Array<IHolder>())` form binds a fresh, unnamed
// `Array` literal as the new `Items` value — its lifetime ends at the
// `with(...)` expression boundary, so the resulting `Box` immediately
// outlives the borrow's source (a dangling borrow, UAF at scope end).
//
// PRE-FIX #4344: `checkRecordFromTemporary` walks `NewExpr::args` only,
// so the `with(...)` initializer was never examined; the program
// compiled clean and would free the `Array` while the `Box` still
// referenced it.
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344, local_decl_temporary_rejected) {
    std::string src =
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: Array<IHolder>, N: int); "
        "class C "
        "  public static method Make(): int "
        "    using var a1 := new Array<IHolder>(); "
        "    a1.Add(new Widget(11)); "
        "    var b1 := new Box(a1, 7); "
        "    var b2 := b1 with(Items := new Array<IHolder>()); "
        "    return b2.N; "
        "  end Make; "
        "  public static method Main(): int return 0; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(src, "§9.7"));
    BEZEL_EXPECT_TRUE(hasError(src, "borrow-bearing record"));
    BEZEL_EXPECT_TRUE(hasError(src, "via `with(...)`"));
    BEZEL_EXPECT_TRUE(hasError(src, "`Items`"));
}

// ===========================================================================
// DIRECTION 1b — RETURN site. Same reject fires from `return r with(...)`
// as from `var x := r with(...)` (handleReturn's twin arm).
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344, return_temporary_rejected) {
    std::string src =
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: Array<IHolder>, N: int); "
        "class C "
        "  public static method Make(): Box "
        "    using var a1 := new Array<IHolder>(); "
        "    a1.Add(new Widget(11)); "
        "    return new Box(a1, 7) with(Items := new Array<IHolder>()); "
        "  end Make; "
        "  public static method Main(): int return 0; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(src, "§9.7"));
    BEZEL_EXPECT_TRUE(hasError(src, "borrow-bearing record"));
    BEZEL_EXPECT_TRUE(hasError(src, "via `with(...)`"));
}

// ===========================================================================
// DIRECTION 1c — ASSIGNMENT site. Same reject fires from `x := r with(...)`
// (handleAssignment's twin arm).
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344, assignment_temporary_rejected) {
    std::string src =
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: Array<IHolder>, N: int); "
        "class C "
        "  public static method Make(): int "
        "    using var a1 := new Array<IHolder>(); "
        "    a1.Add(new Widget(11)); "
        "    var b1 := new Box(a1, 7); "
        "    using var b2 := new Box(new Array<IHolder>(), 0); "
        "    b2 := b1 with(Items := new Array<IHolder>()); "
        "    return b2.N; "
        "  end Make; "
        "  public static method Main(): int return 0; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(src, "§9.7"));
    BEZEL_EXPECT_TRUE(hasError(src, "borrow-bearing record"));
    BEZEL_EXPECT_TRUE(hasError(src, "via `with(...)`"));
}

// ===========================================================================
// DIRECTION 2 — CONTROL: NAMED-LOCAL override is accepted.
//
// §9.7 / §28.3 keep long-lived-borrow construction legal: the override
// value is bound to a named local whose scope outlives the resulting
// record, so the borrow it carries has a bounded source. The new check
// (like its `new Record(...)` twin) treats a named local as a non-
// temporary place, so it is silently admitted.
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344, named_local_override_accepted) {
    auto r = runAot(
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: Array<IHolder>, N: int); "
        "class C "
        "  public static method Main(): int "
        "    using var a1 := new Array<IHolder>(); "
        "    using var w1 := new Widget(11); "
        "    a1.Add(w1); "
        "    var b1 := new Box(a1, 7); "
        "    using var a2 := new Array<IHolder>(); "
        "    using var w2 := new Widget(13); "
        "    a2.Add(w2); "
        "    var b2 := b1 with(Items := a2); "
        "    return b2.Items[0].GetV() + b2.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// ===========================================================================
// DIRECTION 3 — CONTROL: VALUE-TYPED override is accepted.
//
// §9.7 / §28.3 only fires when the OVERRIDE TARGET field is borrow-
// carrying. `Box(Items: Array<IHolder>, N: int)` — overriding the int `N`
// field with an `int` literal/identifier is value-typed, no borrow
// involved, no reject. Guards that the new walk does not over-flag a
// non-borrow-carrying field.
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344, value_field_override_accepted) {
    auto r = runAot(
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: Array<IHolder>, N: int); "
        "class C "
        "  public static method Main(): int "
        "    using var a1 := new Array<IHolder>(); "
        "    using var w1 := new Widget(11); "
        "    a1.Add(w1); "
        "    var b1 := new Box(a1, 7); "
        "    var b2 := b1 with(N := 99); "
        "    return b2.Items[0].GetV() + b2.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 110);
}

// ===========================================================================
// DIRECTION 4 — CONTROL: existing inline-array case from
// tests/sema/with_expr_collection_literal_element_interface_conformance_
// 4330_test.cpp still passes.
//
// `narrow #4330`'s `positional_field_conforming_accepted_control` and
// `body_record_explicit_field_conforming_accepted_control` tests confirm
// that an INLINE-array `[1]IHolder`-typed `with`-clause replacement value
// (the literal `[new Widget3Ok(s)]`) is accepted — §9.7's
// "borrow-bearing record may not be constructed from a temporary" rule
// does NOT fire for inline arrays of references because the literal's
// elements move into the new record's inline-array field at the `with`
// boundary (the record OWNS the elements, not borrows them).
//
// We pin this here so a future fix that widens the §9.7 predicate to
// also reject inline arrays (mirroring `checkRecordFromTemporary`'s
// over-eager behavior for `new Box([new Widget3Ok(s)])`) would have to
// either update #4330's test or document the divergence from
// `checkRecordFromTemporary` — the asymmetry is intentional per #4344's
// rationale (LANE_RULES §6 / regression-test-pins-the-bug).
// ===========================================================================
BEZEL_TEST(with_borrow_carrying_field_temp_4344,
           inline_array_temporary_override_accepted_no_section_9_7) {
    // Verify NO §9.7 reject is emitted for the inline-array case.
    std::string src =
        "module M; "
        "interface IHolder method GetV(): int; end IHolder; "
        "class Widget: IHolder "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public method GetV(): int return V; end GetV; "
        "end Widget; "
        "record Box(Items: [1]IHolder, N: int); "
        "class C "
        "  public static method Main(): int "
        "    var items1: [1]IHolder := [new Widget(11)]; "
        "    var b1 := new Box(items1, 7); "
        "    var b2 := b1 with(Items := [new Widget(13)]); "
        "    return b2.Items[0].GetV() + b2.N; "
        "  end Main; "
        "end C;";
    // The §9.7 reject must NOT be present (the inline-array case is
    // exempt — see the comment above).
    BEZEL_EXPECT_FALSE(hasError(src, "borrow-bearing record"));
    BEZEL_EXPECT_FALSE(hasError(src, "via `with(...)`"));
}

}  // namespace

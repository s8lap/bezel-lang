// Spec-derived regression test for issue #4344 — §9.7 / §28.3 (v0.298):
// record `with(name := expr, …)` non-destructive-update value bound to a
// borrow-carrying field must NOT accept a temporary as the override.
//
// The §9.7 reject "a borrow-bearing record may not be constructed from a
// temporary bound to one of its borrow-carrying fields" already covered
// `new Record(<temporary>)` via `MethodOwnership::checkRecordFromTemporary`
// (`src/ownership/Ownership_Checks.cpp`). The sibling rule for the
// `with(...)` non-destructive update was deferred from `#4330`'s narrowing
// (its original fix) to this issue because the two forms have structurally
// different argument shapes:
//
//   - `NewExpr::args` is positional and indexed against
//     `recordPositionalFieldSymbols(recTs)`.
//   - `WithExpr::assigns` is name-keyed (`InitAssign{name, value}`), so the
//     override must be matched to its borrow-carrying field by walking the
//     source record's `members` and finding the entry whose `name` matches.
//
// Before #4344 the `with(...)` path was routed through `checkRecordFromTemporary`'s
// positional logic only for the `NewExpr` it implicitly expanded into at
// codegen, never at the ownership pass — so a `with(Items := new Array<IHolder>())`
// was accepted by sema+ownership, codegen materialized a `new Holder` whose
// `Items` field was bound to a fresh heap-allocated `Array`, and at scope
// end the temporary Array was freed while the resulting record still held
// the (now-dangling) borrow. The issue's repro measured: compiles clean
// (zero diagnostics), exits 0 in non-ASan mode, `Items[0].GetV()` returns
// the right value only because the heap allocator had not yet coalesced
// the freed block.
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
// §9.7 / §28.3: a record `Box(Items: Array<IHolder>, N: int)` whose `Items`
// field is borrow-carrying. A `with(Items := new Array<IHolder>())` form
// binds a fresh, unnamed `Array` literal as the new `Items` value — its
// lifetime ends at the `with(...)` expression boundary, so the resulting
// `Box` immediately outlives the borrow's source (a dangling borrow, UAF
// at scope end).
//
// PRE-FIX #4344: `checkRecordFromTemporary` walks `NewExpr::args` only,
// so the `with(...)` initializer was never examined; the program compiled
// clean and freed the `Array` while the `Box` still referenced it.
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

} // namespace
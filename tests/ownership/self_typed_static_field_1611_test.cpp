// Spec-derived tests for #1611 / §9.7 (borrow-bearing records) + §9.2 (v0.182
// static-member exemption): `MethodOwnership::isBorrowBearingRecord` walks a
// record's fields to decide whether it carries a borrow, and — before this
// fix — recursed into a field's own record type with NO exclusion for STATIC
// fields and no cycle guard. §9.2 (v0.182) explicitly sanctions a record
// carrying an `Origin`-style `static readonly` field of its own type ("a
// record may carry Origin-style constants and static factories without
// losing `="; "the same static-member exemption applies wherever this
// membership rule is reused") — so a record following this documented
// pattern made the walk recurse into the same record forever, stack-
// overflowing the compiler (SIGSEGV) on a spec-legal program. The fix skips
// STATIC fields in the walk (mirroring Resolver::recordValueContainsSelf's
// existing static exclusion, TypeResolution_Checks.cpp) and threads a
// visited-decl set through the transitive recursion as a defensive guard.
//
// §9.2 (v0.182): "Static members are ignored... a record may carry
// `Origin`-style constants and static factories without losing `=`. The same
// static-member exemption applies wherever this membership rule is reused."
// §9.7: "A record type is a borrow-bearing record... if it holds at least one
// borrow-carrying field... Its type is another borrow-bearing record
// (transitivity)."
//
// Each positive control below is paired with an existing-mechanism negative
// control (§9.7's already-tested borrow-bearing-from-temporary rejection,
// #1066) to confirm the added guard does not change the answer for a
// genuinely borrow-bearing record — only stops the infinite recursion.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

// ---- #1611 crash fix: a self-typed static field must compile, not crash ----

// The issue's exact repro (§9.2's own `Origin`-style canonical example): a
// body record whose only body-record storage is two auto-properties (parsed
// as PropertyDecl, never walked by isBorrowBearingRecord's FieldDecl-only
// member loop) plus a `static readonly` field of the record's OWN type. Prior
// to the fix, checking this record's borrow-bearing status (triggered here by
// `handleLocalDecl`'s borrow-lifetime check on `var a := new Point(1, 2);`)
// recursed into the same TypeDecl without bound and stack-overflowed the
// compiler. Must now compile with NO diagnostics.
BEZEL_TEST(self_typed_static_field_1611, origin_style_static_field_compiles) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Point "
        "  public readonly property X: int; "
        "  public readonly property Y: int; "
        "  public static readonly Origin: Point := new Point(0, 0); "
        "  public constructor(x: int, y: int) "
        "    X := x; "
        "    Y := y; "
        "  end constructor; "
        "end Point; "
        "class C "
        "  public method Run() "
        "    var a := new Point(1, 2); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A MUTUAL static-field cycle (A's static field has type B, B's static field
// has type A) exercises the visited-set across a two-hop chain rather than a
// direct self-reference, confirming the guard generalizes beyond the single-
// decl case. Both records are otherwise value-typed (int-only storage), so
// this must also compile clean.
BEZEL_TEST(self_typed_static_field_1611, mutual_static_field_cycle_compiles) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record A "
        "  public readonly property V: int; "
        "  public static readonly Peer: B := new B(1); "
        "  public constructor(v: int) V := v; end constructor; "
        "end A; "
        "record B "
        "  public readonly property W: int; "
        "  public static readonly Peer: A := new A(2); "
        "  public constructor(w: int) W := w; end constructor; "
        "end B; "
        "class C "
        "  public method Run() "
        "    var a := new A(1); "
        "    var b := new B(2); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- Classification is unchanged: a genuinely borrow-bearing record is ----
// ---- still correctly rejected when constructed from a temporary (#1066) ----

// `Bag` (the #1066 repro type, a POSITIONAL record — §9.1 — whose single
// field is class-typed `Array<int>`) is a real borrow-carrying field per
// §9.7, so `isBorrowBearingRecord(Bag)` must still return true and the
// existing #1066 no-temporary-source rule must still fire. This pins that
// the added static-field skip / visited-set guard does not suppress a
// genuine borrow-bearing classification; only the self-/mutually-
// referential STATIC path (which #1611's fix newly excludes) is affected.
BEZEL_TEST(self_typed_static_field_1611,
           genuine_borrow_bearing_record_from_temporary_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Bag(Items: Array<int>); "
        "class C "
        "  public method Run() "
        "    var b := new Bag([1, 2, 3]); "  // temporary → rejected
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same field bound to a named local/parameter (a place whose scope
// outlives the record) stays ACCEPTED — the positive control pinning that
// #1611's fix leaves the ordinary (non-cyclic) borrow-bearing path fully
// intact.
BEZEL_TEST(self_typed_static_field_1611,
           genuine_borrow_bearing_record_from_place_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Bag(Items: Array<int>); "
        "class C "
        "  public method Run(xs: Array<int>) "
        "    var b := new Bag(xs); "  // place → accepted
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

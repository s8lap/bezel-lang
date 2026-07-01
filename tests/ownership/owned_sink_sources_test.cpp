// Spec-derived tests for the owned-sink borrow-SOURCE classifier gaps that
// caused runtime double-frees (issue #407, with #403 folded in).
//
// §28.3 producers table (the source forms this file pins down):
//   "Indexer access `coll[i]` | Borrowed (source: `coll`)"
//   "Field access `obj.f` where `f` is not `owned` | Borrowed (source: `obj`)"
//   "`this` | Borrowed (source: caller)"
//   "`move coll[i]` where `coll` is an owning collection … | Owned." (§28.8.1)
//   "`move obj.f` where `f` is `owned` | Owned." (§28.8.1)
// §28.3 borrow-lifetime rules: "A borrowed local may not be assigned to any
//   field of any object — neither owned nor non-owned."
// §28.8 (ownership transfer): an `owned` parameter / `set(owned)` sink takes
//   responsibility for disposing the value; only an OWNED value may flow in —
//   "a borrow may not be passed where an owned value is required."
// §28.11 (collection element ownership): the indexer-get on an owning
//   collection "produces a borrow of the slot's value … `move coll[i]` is the
//   explicit-take form."
//
// The under-enforcement this file fixes: `ownedArgSourceIsBorrow` classified
// only bare identifiers and bare *owned*-field reads as borrows; an indexer-get
// `coll[i]`, a *non-owned* field read `obj.f`, and `this` were never flagged, so
// the borrow flowed into an owned field / owned parameter / owned setter and was
// double-disposed at runtime (a deterministic SIGSEGV — confirmed by AOT).
//
// NEGATIVES below must now be REJECTED (they fail-to-reject before the fix);
// POSITIVES must stay ACCEPTED (no over-rejection of the legal `move`/`new`/
// owned-call producers, nor of a borrow used in a borrow-consuming position).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// A `Sink` modeling the owning-collection surface from stdlib §4.5 / §4.9:
// `Add(owned item: T)` and a `set(owned)` indexer. A `Reader.Look(x: T)` is a
// borrow-consuming (non-owned) position. `Pair` exposes a non-owned class-typed
// field `_back: T` (a §28.3 "field access where `f` is not `owned`" borrow
// source). The owned obligations inside bodies are discharged so the helper
// types surface no unrelated diagnostics.
std::string mod(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Pair "
        "  public _back: T := new T(); "
        "end Pair; "
        "class Sink "
        "  private owned _slot: T := new T(); "
        "  public method Add(owned item: T) "
        "    using var hold := move item; "
        "  end Add; "
        "  public property [i: int]: T "
        "    get => this._slot; "
        "    set(owned) => do "
        "      using var hold := move value; "
        "    end; "
        "  end; "
        "end Sink; "
        "class Reader "
        "  public method Look(x: T) _ := 1; end Look; "
        "end Reader; ") + body;
}

} // namespace

// ============================ NEGATIVES ===================================
// Must now be REJECTED (were wrongly accepted before #407).

// ---- Finding [18]: indexer borrow `coll[i]` into an OWNED FIELD -----------
// §28.3 / §28.11 / §28.5: `this._held := coll[i];` reads a slot of an owning
// collection — a borrow (§28.11 "indexer-get produces a borrow of the slot")
// — and stores it into the owned field `_held`. §28.3 forbids assigning a
// borrow to any field; both the field's owner and the collection's `Dispose`
// would dispose the same value (a double-free). REJECTED.
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_owned_field_rejected_28_3) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class Holder "
        "  public owned _held: R; "
        "  public constructor(arr: OwnedArray<R>) "
        "    this._held := arr[0]; "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var arr := new OwnedArray<R>(); "
        "    arr.Add(new R()); "
        "    using var h := new Holder(arr); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- Finding [19]: indexer borrow `coll[i]` into an OWNED ARGUMENT --------
// §28.3 / §28.8 / §28.11: `b.Add(a[0])` passes a slot-borrow of `a` to the
// owned sink `OwnedArray<R>.Add(owned item)`. A borrow handed to an owned sink
// is double-disposed (the sink AND `a`'s `Dispose` both dispose it). REJECTED.
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_owned_arg_builtin_rejected_28_8) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new OwnedArray<R>(); "
        "    a.Add(new R()); "
        "    using var b := new OwnedArray<R>(); "
        "    b.Add(a[0]); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8: the same indexer-borrow into a USER-defined owned sink method (not the
// stdlib `Add`), confirming the gap was not `Add`-specific. REJECTED.
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_owned_arg_user_method_rejected_28_8) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(s: Sink, a: OwnedArray<T>) "
        "    s.Add(a[0]); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.11 (#395 routing through the extended classifier): the indexer-borrow as
// the RHS of an owning-collection `set(owned)` slot — a borrow into an owned
// slot. REJECTED.
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_owned_setter_rejected_28_11) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(s: Sink, a: OwnedArray<T>) "
        "    s[0] := a[0]; "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8: an indexer-borrow into an owned CONSTRUCTOR argument. REJECTED.
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_owned_ctor_arg_rejected_28_8) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class Box "
        "  public constructor(owned x: R) "
        "    using var hold := move x; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public method Use(a: OwnedArray<R>) "
        "    using var box := new Box(a[0]); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- Finding [20]: NON-owned class-field read `obj.f` into an OWNED FIELD --
// §28.3 / §28.5: `this._held := p._back;` reads a NON-owned class-typed field
// `_back` (a §28.3 "field access where `f` is not `owned`" borrow, source `p`)
// and stores it into the owned field `_held`. §28.3 forbids storing a borrow in
// any field; the same R is disposed by `_held`'s owner and by `p`'s actual
// owner — a double-free. REJECTED. (The owned-field-read variant was already
// caught; this pins down the non-owned-field case.)
BEZEL_TEST(owned_sink_sources, nonowned_field_read_into_owned_field_rejected_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "class Sink2 "
        "  public owned _held: T; "
        "  public constructor(owned first: T) this._held := first; end constructor; "
        "  public method Take(p: Pair) "
        "    this._held := p._back; "
        "  end Take; "
        "end Sink2; "
        "class C end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8: a non-owned class-field read passed to an owned sink argument. The
// borrow (source: `p`) would be double-disposed. REJECTED.
BEZEL_TEST(owned_sink_sources, nonowned_field_read_into_owned_arg_rejected_28_8) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(s: Sink, p: Pair) "
        "    s.Add(p._back); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- #403: bare `this` into an owned sink --------------------------------
// §28.3 / §28.8: `this` is Borrowed (source: caller) and can never transfer
// ownership of the receiver. Passing it to the owned sink `Add(owned)` would
// let the sink dispose the live receiver the caller still owns — a double-
// dispose. REJECTED.
BEZEL_TEST(owned_sink_sources, this_into_owned_arg_rejected_28_3) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R "
        "  public method Leak(s: OwnedArray<R>) "
        "    s.Add(this); "
        "  end Leak; "
        "end R; "
        "class C end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 / §28.5: bare `this` stored into an owned field — same hazard, owned-
// field destination. REJECTED. The owned field is nullable with a `null`
// initializer so the §7.9 "field needs an initializer" rule does not fire; the
// only diagnostic is the §28.3 borrow-into-owned-field one this fix adds, making
// the test load-bearing for the `this`-source classification specifically.
BEZEL_TEST(owned_sink_sources, this_into_owned_field_rejected_28_3) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R "
        "  public owned _self: R? := null; "
        "  public method Wire() "
        "    this._self := this; "
        "  end Wire; "
        "end R; "
        "class C end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ============================ POSITIVES ===================================
// Must stay ACCEPTED — the load-bearing over-rejection guards.

// §28.3 / §28.8.1 / §28.11: `move coll[i]` is the OWNED slot-take producer (the
// slot is left moved-from). Passing it to `Add(owned)` is a legal transfer.
// ACCEPTED. (Without `move`, this is finding [19]; with `move`, it is legal —
// the load-bearing distinction this fix preserves.)
BEZEL_TEST(owned_sink_sources, move_indexer_into_owned_arg_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new OwnedArray<R>(); "
        "    a.Add(new R()); "
        "    using var b := new OwnedArray<R>(); "
        "    b.Add(move a[0]); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8.1: `move this._owned` is the OWNED owned-field-take producer
// (the only way to read an owned field as owned); reassign the field after.
// Passing it to `Add(owned)` is a legal transfer. ACCEPTED.
BEZEL_TEST(owned_sink_sources, move_owned_field_into_owned_arg_accepted_28_8_1) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class Holder "
        "  public owned _r: R := new R(); "
        "  public method Give(a: OwnedArray<R>) "
        "    a.Add(move this._r); "
        "    this._r := new R(); "
        "  end Give; "
        "end Holder; "
        "class C end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 ("A `new` expression produces an owned value"): `new R()` into an owned
// field is the canonical legal transfer. ACCEPTED.
BEZEL_TEST(owned_sink_sources, new_into_owned_field_accepted_28_8) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class Holder "
        "  public owned _r: R; "
        "  public constructor() this._r := new R(); end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: an indexer-borrow `a[0]` used in a BORROW-consuming position — passed
// to the non-owned parameter `Look(x: T)`. The slot keeps its owner; nothing is
// transferred. Must NOT be flagged as an owned-sink violation. ACCEPTED.
// (The load-bearing over-rejection guard for the indexer classification.)
BEZEL_TEST(owned_sink_sources, indexer_borrow_into_borrow_param_accepted_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(rd: Reader, a: OwnedArray<T>) "
        "    rd.Look(a[0]); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: a non-owned field read `p._back` in a BORROW-consuming position. The
// field's owner keeps it; nothing is transferred. ACCEPTED.
// (The load-bearing over-rejection guard for the non-owned-field classification.)
BEZEL_TEST(owned_sink_sources, nonowned_field_read_into_borrow_param_accepted_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(rd: Reader, p: Pair) "
        "    rd.Look(p._back); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: `this` in a BORROW-consuming position — passed to a non-owned
// parameter. `this` is a legal borrow there. Must NOT be flagged. ACCEPTED.
// (The load-bearing over-rejection guard for the `this` classification.)
BEZEL_TEST(owned_sink_sources, this_into_borrow_param_accepted_28_3) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R "
        "  public method Look(x: R) _ := 1; end Look; "
        "  public method Run() "
        "    this.Look(this); "
        "  end Run; "
        "end R; "
        "class C end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8: the §28.8 owned-field-assignment pattern `this._x := value;` where
// `value` is a consumed `owned` parameter (an OWNED value, not a borrow). The
// caller relinquished ownership; the field takes it over. Must stay ACCEPTED —
// the classifier must not over-reject a legal owned-parameter source. ACCEPTED.
BEZEL_TEST(owned_sink_sources, owned_param_into_owned_field_accepted_28_8) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class Sink2 "
        "  public owned _x: R; "
        "  public constructor(owned v: R) this._x := v; end constructor; "
        "end Sink2; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Sink2(new R()); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 move-semantics: a `move` of an owned LOCAL into an owned sink remains
// the canonical legal transfer — a regression guard alongside the new forms.
// ACCEPTED.
BEZEL_TEST(owned_sink_sources, move_owned_local_into_owned_arg_accepted_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  public method Use(s: Sink) "
        "    var x := new T(); "
        "    s.Add(move x); "
        "  end Use; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §28.8 (#1280): a record-DESTRUCTURING pattern binding is a
// non-destructive BORROW of the matched field (the scrutinee's owner still owns
// the value). Passing it to an `owned` sink lets the sink dispose a value the
// real owner also disposes — a double-free. It must be rejected exactly as a
// plain borrow local is. (Before the fix the ownership pass did not walk case-arm
// bodies at all, so the §28.8 check never fired there.) REJECTED.
BEZEL_TEST(owned_sink_sources, destructure_binding_into_owned_arg_rejected_1280) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget end Widget; record Box(A: Widget); "
        "class Prog "
        "  public static method Sink(owned w: Widget) end Sink; "
        "  public static method Use(b: Box) "
        "    case b Box(a) => Prog.Sink(a) end; "
        "  end Use; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8 (#1280) broader: a borrowed PARAMETER passed to an `owned` sink INSIDE a
// case-statement arm is now caught (case-arm bodies are walked). REJECTED.
BEZEL_TEST(owned_sink_sources, borrow_param_into_owned_arg_in_case_arm_rejected_1280) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget end Widget; "
        "class Prog "
        "  public static method Sink(owned w: Widget) end Sink; "
        "  public static method Use(p: Widget, n: int) "
        "    case n 0 => Prog.Sink(p), _ => do end, end; "
        "  end Use; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.3 (#1280) control: merely READING a destructure binding in the arm (not
// handing it to an `owned` sink) is a legal borrow use. ACCEPTED.
BEZEL_TEST(owned_sink_sources, destructure_binding_read_in_arm_accepted_1280) {
    auto r = parseAndCheckOwnership(
        "module M; record Inner(v: int); record Outer(i: Inner); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(9)); "
        "  return case o Outer(Inner(x)) => x, _ => 0, end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 (#1280) control: a genuine owned transfer (`move` of an owned local)
// INSIDE a case arm still compiles — the new case-arm walk does not over-reject.
BEZEL_TEST(owned_sink_sources, owned_move_into_owned_arg_in_case_arm_accepted_1280) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget end Widget; "
        "class Prog "
        "  public static method Sink(owned w: Widget) end Sink; "
        "  public static method Use(n: int) "
        "    case n 0 => do var w := new Widget(); Prog.Sink(move w); end, "
        "           _ => do end, end; "
        "  end Use; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / §28.8 (#1284, expression-position sibling of #1280): a borrow handed to
// an `owned` sink inside a case-EXPRESSION arm (`return case b Box(a) =>
// Prog.Sink(a), _ => -1 end`) must be rejected too — the ownership pass now walks
// CaseExpr arm bodies (handleDischarge), not only CaseStmt arms. REJECTED.
BEZEL_TEST(owned_sink_sources, destructure_binding_into_owned_arg_case_expr_rejected_1284) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget end Widget; record Box(A: Widget); "
        "class Prog "
        "  public static method Sink(owned w: Widget): int return 0; end Sink; "
        "  public static method Use(b: Box): int "
        "    return case b Box(a) => Prog.Sink(a), _ => -1, end; "
        "  end Use; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8 (#1284) control: a genuine owned producer (`new`) passed to an `owned`
// sink inside a case-EXPRESSION arm still compiles — no over-rejection. ACCEPTED.
BEZEL_TEST(owned_sink_sources, owned_new_into_owned_arg_case_expr_accepted_1284) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget end Widget; "
        "class Prog "
        "  public static method Sink(owned w: Widget): int return 0; end Sink; "
        "  public static method Use(n: int): int "
        "    return case n 0 => Prog.Sink(new Widget()), _ => -1, end; "
        "  end Use; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

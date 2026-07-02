// Spec-derived tests for the owned-FIELD-read-into-owned-sink hole
// (issue #398, follow-up to #395).
//
// §28.3 producers table:
//   "Field access `obj.f` where `f` is `owned` | Borrowed (source: `obj`)"
//   "`move obj.f` where `f` is `owned` | Owned. Field is left in moved-from
//    state and must be reassigned before the surrounding method exits
//    (§28.8.1)."
// §28.8 (ownership transfer): an `owned` parameter / `set(owned)` sink takes
//   responsibility for disposing the value; only an OWNED value may flow in.
// §28.8.1 (the `move` expression): "`move x.f` … is the only way to read an
//   owned field's value as an owned value; all other reads of an owned field
//   produce a borrow (§28.3)."
//
// The under-enforcement this file pins down (and #398 fixes): a BARE owned-
// field read — `s.Add(this._owned)` with `_owned` an owned field, no enclosing
// `move` — is a BORROW per §28.3, not an owned value. Handing it to an owned
// sink would let the sink dispose a value the field's owner (this) also
// disposes — a double-dispose. The correct form is `move this._owned` (an
// owned producer per §28.3 / §28.8.1, which also leaves the field moved-from).
//
// NEGATIVES below must now be REJECTED; POSITIVES must stay ACCEPTED (no
// over-rejection of the legal `move`-of-field producer, nor of a borrow-
// consuming read of the owned field).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `class C` holds an owned field `_owned: T`. `Sink` is the owning-collection
// surface from stdlib §4.5 / §4.9: `Add(owned item: T)` and a `set(owned)`
// indexer. `Inspector.Peek(x: T)` is a borrow-consuming (non-owned) position.
// The owned obligations inside the bodies are discharged so the helper types
// surface no unrelated diagnostics.
std::string mod(const std::string& method) {
    return std::string("module M; "
        "class T end T; "
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
        "class Inspector "
        "  public method Peek(x: T) _ := 1; end Peek; "
        "end Inspector; "
        "class C "
        "  private owned _owned: T := new T(); "
        ) + method + " end C;";
}

} // namespace

// ---- NEGATIVE cases: a bare owned-field read into an owned sink ----------
// These must FAIL-to-reject before the #398 fix and be REJECTED after it.

// §28.3 / §28.8: `s.Add(this._owned)` reads the owned field `_owned` WITHOUT
// `move` — a borrow (§28.3 "Field access obj.f where f is owned | Borrowed").
// Passing that borrow to the owned sink `Add(owned item)` would double-dispose
// (the sink AND `this` both dispose the field's value). REJECTED.
BEZEL_TEST(owned_field_sink, add_bare_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  s.Add(this._owned); "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8: the same hazard with an explicit `owned` call-site modifier on the
// bare owned-field read — still a borrow source, still REJECTED.
BEZEL_TEST(owned_field_sink, add_owned_modifier_on_bare_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  s.Add(owned this._owned); "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8 / §28.11: the owning-collection `set(owned)` indexer with a bare
// owned-field RHS — a borrow into an owned slot. The slot would dispose a
// value `this` also owns. REJECTED.
BEZEL_TEST(owned_field_sink, indexer_set_bare_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  s[0] := this._owned; "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8: a bare owned-field read passed as the `owned` argument of a
// *constructor* is the same hazard. REJECTED.
BEZEL_TEST(owned_field_sink, new_ctor_bare_owned_field_rejected) {
    auto r = parseAndCheckOwnership(std::string("module M; class T end T; "
        "class Box "
        "  public constructor(owned x: T) "
        "    using var hold := move x; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  private owned _owned: T := new T(); "
        "  public method M() "
        "    using var box := new Box(this._owned); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// stdlib §4.5: a bare owned-field read handed to the BUILT-IN
// `OwningArray<T>.Add(owned item)` is the headline #398 case — a borrow into an
// owned sink. REJECTED. (Built-in collections have no AST declaration, so the
// owned sink is recognized by receiver-type + method name, as in #395.)
BEZEL_TEST(owned_field_sink, builtin_array_add_bare_owned_field_rejected) {
    auto r = parseAndCheckOwnership(std::string("module M; class R end R; "
        "class C "
        "  private owned _owned: R := new R(); "
        "  public method M(a: OwningArray<R>) "
        "    a.Add(this._owned); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- POSITIVE cases: must stay ACCEPTED (no over-rejection) --------------

// §28.3 / §28.8.1: `move this._owned` is an OWNED producer — the only way to
// read an owned field's value as an owned value. Passing it to `Add(owned)` is
// a legal transfer; the field is left moved-from and must be reassigned before
// method exit (here, `this._owned := new T();`). ACCEPTED.
BEZEL_TEST(owned_field_sink, add_move_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  s.Add(move this._owned); "
        "  this._owned := new T(); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8.1: the `set(owned)` indexer with a `move`-of-owned-field RHS is
// the legal owned transfer into a slot; reassign the field afterward. ACCEPTED.
BEZEL_TEST(owned_field_sink, indexer_set_move_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  s[0] := move this._owned; "
        "  this._owned := new T(); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: reading the owned field in a BORROW-consuming position — passing it to
// the non-owned parameter `Peek(x: T)` — is a legal borrow (§28.3 "Field access
// obj.f where f is owned | Borrowed"). It must NOT be flagged as an owned-sink
// violation: the field keeps its ownership; nothing is transferred. ACCEPTED.
// (This is the load-bearing over-rejection guard for the #398 classification.)
BEZEL_TEST(owned_field_sink, borrow_consuming_owned_field_read_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(i: Inspector) "
        "  i.Peek(this._owned); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 move-semantics: a `move` of an owned LOCAL into the owned sink remains
// the canonical legal transfer (regression guard alongside the field change).
// ACCEPTED.
BEZEL_TEST(owned_field_sink, add_move_owned_local_still_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method M(s: Sink) "
        "  var x := new T(); "
        "  s.Add(move x); "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Spec-derived tests for the borrow-into-owned-FIELD assignment hole
// (issue #402, follow-up to #395 / #398 / #386).
//
// §28.3 producers table (the authority for "what is Owned vs Borrowed"):
//   "`new T(...)` | Owned"
//   "Method call returning `owned T` | Owned"
//   "Field access `obj.f` where `f` is `owned` | Borrowed (source: `obj`)"
//   "`move obj.f` where `f` is `owned` | Owned. Field is left in moved-from
//    state and must be reassigned before the surrounding method exits (§28.8.1)."
//   "Parameter `p` declared `owned p: T` | Owned"
//   "Parameter `p` (no modifier) | Borrowed (source: caller)"
//   "Local-variable read `x` where `x` is borrowed | Borrowed"
// §28.5 (owned fields): "A field marked `owned` belongs to the containing
//   object exclusively" and is auto-disposed when the object is disposed.
// §28.8 (ownership transfer): the canonical owned-field assignment
//   `_resource := resource;` where `resource` is an `owned` parameter — the
//   field receives an OWNED value (the relinquished parameter).
//
// The under-enforcement this file pins down (and #402 fixes): an owned-field
// ASSIGNMENT `this._owned := RHS` whose RHS is a BORROW — a bare borrow
// parameter (the headline constructor case), a borrowed local, or a bare
// (un-`move`d) read of another owned field — stores a value the field's owner
// (this) will dispose, while the borrow's real owner ALSO disposes it: a
// double-free / use-after-free at runtime. Per §28.3 / §28.5 / §28.8 an owned
// field may only receive an OWNED value: a `new`, a `move` of an owned
// local/field, an owned-returning call, or a consumed `owned` parameter.
//
// This is the field-write analogue of #395 (borrow into an owned parameter /
// setter) and #398 (bare owned-field read into an owned sink). It composes with
// #386 (`checkBorrowStoreEscape`), which flags storing a borrow of an owned
// scope-bound *local* into ANY field; #402 covers the disjoint borrow sources
// (borrow parameter, bare owned-field read) flowing specifically into an OWNED
// field. The two checks do not double-report (see the borrow-local case below).
//
// NEGATIVES below must now be REJECTED; POSITIVES must stay ACCEPTED (no
// over-rejection of any legal owned transfer into an owned field).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `class Wrap` holds an owned field `_c: Inner`, initialized so the field has a
// disposed-then-overwrite or fresh value where needed. `Inner` is a trivial
// class. `MakeOwned()` is the owned-returning factory (§28.3 "Method call
// returning `owned T` | Owned"). `_other` is a second owned field for the
// field-to-field cases. Method/ctor bodies discharge their own obligations so
// the helper surfaces no unrelated diagnostics.
std::string mod(const std::string& member) {
    return std::string("module M; "
        "class Inner end Inner; "
        "class Wrap "
        "  private owned _c: Inner := new Inner(); "
        "  private owned _other: Inner := new Inner(); "
        "  public static method MakeOwned(): owned Inner "
        "    return new Inner(); "
        "  end MakeOwned; "
        ) + member + " end Wrap;";
}

} // namespace

// ---- NEGATIVE cases: a borrow stored into an owned field -----------------
// These must FAIL-to-reject before the #402 fix and be REJECTED after it.

// §28.3 / §28.8: the headline constructor case. `c` is a BARE borrow parameter
// (no `owned`), so `this._c := c;` stores a borrow into the owned field `_c`.
// At runtime `_c`'s owner (the Wrap) disposes it and the borrow's real owner
// (the caller) disposes it too — a double-free. REJECTED.
BEZEL_TEST(owned_field_assign, ctor_borrow_param_into_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public constructor(c: Inner) "
        "  this._c := c; "
        "end constructor;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8: the same hazard in an ordinary method. `c` is a borrow
// parameter; `this._c := c;` stores the borrow into the owned field. (The field
// is disposed first per §28.7; that does not make `c` an owned value.) REJECTED.
BEZEL_TEST(owned_field_assign, method_borrow_param_into_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Take(c: Inner) "
        "  this._c.Dispose(); "
        "  this._c := c; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3: a borrowed LOCAL stored into an owned field. `var b := c;` makes `b` a
// borrow (it aliases the borrow parameter `c`, whose source is the caller, NOT
// an owned scope-bound local — so #386's `checkBorrowStoreEscape` does not
// fire). `this._c := b;` stores that borrow into the owned field. The #402
// check catches it; exactly one diagnostic is produced. REJECTED.
BEZEL_TEST(owned_field_assign, borrow_local_into_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Take(c: Inner) "
        "  var b := c; "
        "  this._c.Dispose(); "
        "  this._c := b; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8.1: a BARE read of another owned field (`this._other`, no
// `move`) is a borrow ("Field access obj.f where f is owned | Borrowed").
// Storing it into the owned field `_c` would give both fields disposal
// responsibility for the same object — a double-free. The legal form is
// `move this._other` (and then restoring `_other`). REJECTED.
BEZEL_TEST(owned_field_assign, bare_owned_field_read_into_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Swap() "
        "  this._c.Dispose(); "
        "  this._c := this._other; "
        "end Swap;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// NOTE — DEFERRED corner case: `this._self := this;` (storing the receiver, a
// borrow per §28.3 "`this` | Borrowed", into an owned field of the class's own
// type) is also unsound, but `ownedArgSourceIsBorrow` deliberately does NOT
// classify `this` as a borrow (its conservative tail leaves `this`, and any
// expression it cannot positively prove a borrow, unflagged, to avoid over-
// rejecting a legal transfer). Flagging `this` would require widening that
// classifier, which is out of scope for #402's three stated borrow forms
// (borrow parameter, borrowed local, bare owned-field read) and risks broader
// effects. Left for a follow-up; not asserted here so the conservative-and-
// sound contract holds. // per §28.3 (deferred)

// ---- POSITIVE cases: must stay ACCEPTED (no over-rejection) --------------

// §28.3 / §28.8: `new Inner()` is the canonical Owned producer; storing it into
// the owned field is the canonical legal assignment. ACCEPTED. (Dispose the
// previous value first per §28.7 so no undisposed-overwrite warning escalates.)
BEZEL_TEST(owned_field_assign, new_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Refresh() "
        "  this._c.Dispose(); "
        "  this._c := new Inner(); "
        "end Refresh;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8: the canonical §28.8 `TakeOwnership` shape — a consumed `owned`
// parameter assigned to the owned field. The owned parameter IS an owned value
// ("Parameter `p` declared `owned p: T` | Owned"); the caller relinquishes it.
// ACCEPTED. This is the load-bearing over-rejection guard for the headline fix:
// the bare borrow-param case must be rejected while this owned-param case is
// accepted.
BEZEL_TEST(owned_field_assign, owned_param_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method TakeOwnership(owned resource: Inner) "
        "  this._c.Dispose(); "
        "  this._c := resource; "
        "end TakeOwnership;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8: the same as a CONSTRUCTOR — `owned c` consumed into the owned field.
// The constructor's first assignment needs no prior Dispose (§28.8, the field
// "was never set"). ACCEPTED.
BEZEL_TEST(owned_field_assign, ctor_owned_param_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Inner end Inner; "
        "class Wrap "
        "  private owned _c: Inner; "
        "  public constructor(owned c: Inner) "
        "    this._c := c; "
        "  end constructor; "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8 / §28.8.1: `move c` of an `owned` parameter is also an owned value;
// storing it into the owned field is a legal transfer. ACCEPTED.
BEZEL_TEST(owned_field_assign, move_owned_param_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method TakeOwnership(owned resource: Inner) "
        "  this._c.Dispose(); "
        "  this._c := move resource; "
        "end TakeOwnership;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: `move ownedLocal` is an Owned producer (the local moves out and is
// left moved-from). Storing it into the owned field is a legal transfer.
// ACCEPTED.
BEZEL_TEST(owned_field_assign, move_owned_local_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Replace() "
        "  var fresh := new Inner(); "
        "  this._c.Dispose(); "
        "  this._c := move fresh; "
        "end Replace;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: a plain `var` owned local read in an owned-consuming position (the
// owned-field assignment) transfers ownership out of the local — the local is
// left moved-from. The bare `this._c := fresh;` form (no explicit `move`) is
// the move-on-read of §28.3 and is a legal transfer into the owned field.
// ACCEPTED.
BEZEL_TEST(owned_field_assign, owned_local_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Replace() "
        "  var fresh := new Inner(); "
        "  this._c.Dispose(); "
        "  this._c := fresh; "
        "end Replace;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.8.1: `move this._other` is the ONLY way to read an owned field as
// an owned value. Storing it into `_c` is a legal transfer; `_other` is left
// moved-from and must be reassigned before method exit (here, restored).
// ACCEPTED. (Contrast the bare-read negative above.)
BEZEL_TEST(owned_field_assign, move_owned_field_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Swap() "
        "  this._c.Dispose(); "
        "  this._c := move this._other; "
        "  this._other := new Inner(); "
        "end Swap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: a method call returning `owned T` is an Owned producer. Storing its
// result into the owned field is a legal transfer. ACCEPTED.
BEZEL_TEST(owned_field_assign, owned_returning_call_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Refill() "
        "  this._c.Dispose(); "
        "  this._c := Wrap.MakeOwned(); "
        "end Refill;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.7 / §28.3 null-literal row: assigning `null` to a NULLABLE owned field is
// permitted — `null` has no value to own and is compatible with an owned
// target (the field enters the owned-currently-null state). ACCEPTED. This
// guards against a too-broad rule that rejects every non-`new`/non-`move` RHS.
BEZEL_TEST(owned_field_assign, null_into_nullable_owned_field_accepted) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Inner end Inner; "
        "class Wrap "
        "  private owned _c: Inner? := new Inner(); "
        "  public method Clear() "
        "    if this._c <> null then "
        "      this._c.Dispose(); "
        "    end; "
        "    this._c := null; "
        "  end Clear; "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: a write to a NON-owned field (a back-reference / value-typed field) is
// NOT subject to the owned-field rule. Assigning an int to a plain field must
// stay accepted — the #402 rule must fire ONLY for owned-field targets.
// ACCEPTED. (Over-rejection guard: the rule keys on the field's `owned`
// modifier, not on every field write.)
BEZEL_TEST(owned_field_assign, plain_field_write_unaffected_accepted) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Wrap "
        "  private _count: int := 0; "
        "  public method Bump(n: int) "
        "    this._count := n; "
        "  end Bump; "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- EDGE cases for the move-on-read transfer-IN semantics ---------------

// §28.3: a `using var` is owned but scope-bound — it "cannot be moved out of,
// only borrowed from." A bare `this._c := u;` would move `u` out into the owned
// field, which §28.3 forbids ("Cannot move out of `using var`"). REJECTED.
// This guards the transfer-in recording: it must route a bare owned-local read
// into the move-on-read path (which correctly rejects a `using var`), not treat
// it as a free pass into the field.
BEZEL_TEST(owned_field_assign, using_var_into_owned_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Bad() "
        "  using var u := new Inner(); "
        "  this._c.Dispose(); "
        "  this._c := u; "
        "end Bad;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 move semantics: after a bare owned-LOCAL read transfers ownership into
// the owned field, the local is left MOVED-FROM and may not be read again. A
// second use (`return fresh;` under an `owned` return) must be rejected — the
// value already moved into the field. This proves the transfer-in recorded the
// move (not merely accepted it). REJECTED.
BEZEL_TEST(owned_field_assign, owned_local_moved_from_after_field_store_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Bad(): owned Inner "
        "  var fresh := new Inner(); "
        "  this._c.Dispose(); "
        "  this._c := fresh; "
        "  return fresh; "
        "end Bad;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.8 / §28.3: after a bare owned-PARAMETER read is consumed into the owned
// field (the §28.8 `_resource := resource;` pattern), the parameter is
// discharged and may not be used again — a later `move resource` is reading a
// disposed/consumed value. REJECTED. This proves the parameter discharge was
// recorded (and, conversely, that the single legal consume does NOT leave the
// owned parameter undischarged — the accepted positive above).
BEZEL_TEST(owned_field_assign, owned_param_consumed_then_reused_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Bad(owned resource: Inner) "
        "  this._c.Dispose(); "
        "  this._c := resource; "
        "  using var again := move resource; "
        "end Bad;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

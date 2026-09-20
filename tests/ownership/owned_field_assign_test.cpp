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

using bezel::test::OwnershipResult;
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

// v0.303 / #4450 (the #3975 option-1 ruling): under a DOMINATING
// `Dispose()`, every use of the source local — read, borrow, AND consume —
// is §28.2's use-after-dispose WARNING, NOT §28.3's consume-table ERROR.
// The consume-side ruling is the new contract; the OLD §28.3 ERROR text
// ("cannot move from disposed local `x`") is GONE.  These two helpers are
// the inclusive matcher (matches BOTH wording variants — the read-side
// `checkRead` produces and the consume-side `markMovedByName` produces
// post-ruling) and the negative guard that the OLD §28.3 ERROR must NOT
// have reappeared (catches any over-relaxation of the conditional-kill
// arms, which the demotion did NOT touch).
//
// per §28.2, §28.3 K1.3/K7, v0.303, #3975 (option 1), #4450
inline bool hasUseAfterDisposeWarning(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("use of `") != std::string::npos &&
            d.message.find("after a dominating explicit Dispose() call") !=
                std::string::npos)
            return true;
        if (d.message.find("move from a local `") != std::string::npos &&
            d.message.find("already disposed at this point") !=
                std::string::npos)
            return true;
    }
    return false;
}
inline bool hasCannotMoveFromDisposedError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot move from disposed local") !=
                std::string::npos) return true;
    return false;
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
// per §7.4 (#862): the null check uses the null-conditional `?.Dispose()` form
// (§7.5) because fields are not narrowable — the direct `this._c.Dispose()`
// inside `if this._c <> null then` is a §7.4 compile error.
BEZEL_TEST(owned_field_assign, null_into_nullable_owned_field_accepted) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Inner end Inner; "
        "class Wrap "
        "  private owned _c: Inner? := new Inner(); "
        "  public method Clear() "
        "    this._c?.Dispose(); "
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
// disposed/consumed value. v0.303 / #4450 demoted the dominating-kill arm
// from §28.3 ERROR to §28.2 WARNING (the ruling is option 1 of #3975), so
// the verdict MOVED: pre-fix this was a hard REJECT, post-fix it is a
// WARNING admitted by §28.2's checked-disposal backstop.  The OLD §28.3
// "cannot move from disposed local" ERROR text is GONE — the negative
// guard below pins it as absent.
BEZEL_TEST(owned_field_assign, owned_param_consumed_then_reused_warns_28_2_4450) {
    auto r = parseAndCheckOwnership(mod(
        "public method Bad(owned resource: Inner) "
        "  this._c.Dispose(); "
        "  this._c := resource; "
        "  using var again := move resource; "
        "end Bad;"));
    // §28.2 (v0.303) WARNING — the dominating-kill verdict, not the
    // §28.3 consume-table error.  The consume-side wording, distinct from
    // `checkRead`'s read-side text.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    // Negative guard: the OLD §28.3 ERROR text must NOT have re-appeared.
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// ---- §28.6 / #1484: fresh owned value in a FIELD INITIALIZER ---------------
//
// §28.6 (Leak prevention): "A freshly-produced owned value — a `new`
// expression, a call returning `owned T`, a `move` expression, or an owned
// operator result — stored into a non-`owned` (borrow) field is an error: the
// field does not own its referent, so the fresh value would be left with no
// owner and leak." The constructor-body assignment `this.f := new R()` is
// already rejected (handleAssignment's targetIsNonOwnedField branch); the
// FIELD-INITIALIZER form `f: R := new R()` was silently accepted and leaked the
// R (issue #1484 — the FieldDecl member was never ownership-analysed). These
// tests pin the two positions to the identical rule.

// True when some ERROR diagnostic mentions the §28.6 non-owned-field leak text
// (the same message the constructor-body path emits), so the reject cases prove
// the §28.6 diagnostic fired — not some unrelated error.
static bool hasNonOwnedFieldLeakError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("freshly-produced owned value cannot be stored "
                           "into the non-`owned` field") != std::string::npos)
            return true;
    }
    return false;
}

// §28.6 / #1484 NEGATIVE: a `new` (fresh owned producer, §28.3 "`new T(...)` |
// Owned") in the initializer of a NON-`owned` field. The field does not own the
// value, so it leaks — REJECTED, exactly as the constructor-body `f := new R()`
// form is.
BEZEL_TEST(owned_field_assign,
           fresh_new_into_non_owned_field_initializer_rejected_spec_28_6_1484) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R end R; "
        "class Holder "
        "  public r: R := new R(); "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwnedFieldLeakError(r));
}

// §28.6 / #1484 NEGATIVE: an owned-returning CALL (§28.3 "Method call returning
// `owned T` | Owned") in the initializer of a NON-`owned` field is likewise a
// fresh owned producer and leaks — REJECTED. Covers the second producer shape.
BEZEL_TEST(owned_field_assign,
           owned_call_into_non_owned_field_initializer_rejected_spec_28_6_1484) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R "
        "  public static method Make(): owned R "
        "    return new R(); "
        "  end Make; "
        "end R; "
        "class Holder "
        "  public r: R := R.Make(); "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonOwnedFieldLeakError(r));
}

// §28.6 / §28.5 / #1484 POSITIVE: the SAME fresh `new` in the initializer of an
// `owned` field is ACCEPTED — the field owns the value and the containing object
// disposes it (§28.5). This is the boundary the fix must not over-reject.
BEZEL_TEST(owned_field_assign,
           fresh_new_into_owned_field_initializer_accepted_spec_28_6_1484) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class R end R; "
        "class Holder "
        "  public owned r: R := new R(); "
        "  destructor end destructor; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 / #1484 POSITIVE: a `new` of a VALUE type (a record) in a non-`owned`
// field initializer is a plain value with no disposal obligation (§9.7 forbids
// record destructors / owned fields), so producerState classifies it Borrowed —
// NOT a fresh owned producer. ACCEPTED. This proves the reject is gated on
// producerState == Owned, not merely on the `new` keyword.
BEZEL_TEST(owned_field_assign,
           fresh_record_into_non_owned_field_initializer_accepted_spec_28_6_1484) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "record Point(X: int, Y: int); "
        "class Holder "
        "  public p: Point := new Point(1, 2); "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §23.1 / §28.6 / #2168: auto-property backing-field initializer -------
//
// An auto-property's generated backing field is ordinary non-`owned` storage
// (§23.1). Therefore §28.6's fresh-value-to-borrow-field rejection applies at
// this declaration site exactly as it does to an explicit non-`owned` field.
// These controls pin the two valid alternatives: initialize the auto-property
// later from an existing borrow, or put the fresh value in an explicit `owned`
// field whose containing-object disposal cascade owns it (§28.5). // spec v0.226

static bool hasAutoPropertyInitializerLeakError(
        const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("freshly-produced owned value cannot initialize "
                           "auto-property `Current`") != std::string::npos &&
            d.message.find("§28.6") != std::string::npos)
            return true;
    }
    return false;
}

// §23.1 / §28.6 (v0.226) REJECT: `new Widget()` is owned (§28.3 K1.5),
// but Current's generated backing field has no owning marker or cascade.
BEZEL_TEST(owned_field_assign,
           fresh_new_auto_property_initializer_rejected_spec_23_1_28_6_2168) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Widget end Widget; "
        "class Holder "
        "  public readonly property Current: Widget := new Widget(); "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoPropertyInitializerLeakError(r));
}

// §23.1 / §28.3 / §28.6 (v0.226) ACCEPT: an initializer-less auto-property
// remains valid. The constructor stores a caller-owned borrow in its backing
// field; no fresh obligation is abandoned at the declaration site.
BEZEL_TEST(owned_field_assign,
           class_auto_property_without_initializer_accepted_spec_23_1_2168) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Widget end Widget; "
        "class Holder "
        "  public readonly property Current: Widget; "
        "  public constructor(w: Widget) Current := w; end constructor; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.5 / §28.6 (v0.226) ACCEPT: the existing owning-storage pattern remains
// the way to give a fresh field-initializer value a real owner. Holder's
// generated disposal cascade owns and disposes Current.
BEZEL_TEST(owned_field_assign,
           fresh_new_owned_field_initializer_still_accepted_spec_28_5_2168) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "class Widget end Widget; "
        "class Holder "
        "  public owned Current: Widget := new Widget(); "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

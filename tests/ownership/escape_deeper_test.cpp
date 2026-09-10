// Spec-derived tests for issue #386 — deepening the ownership escape analysis
// to the residual INDIRECT-escape forms of a borrow of an owned scope-bound
// local. The existing #360 work catches the *return* forms (direct borrow,
// closure capture, record borrow, and the indirect-return-via-holder case).
// This milestone adds three further escape channels and resolves the
// parameter-source question:
//
//   CASE 1 — store-to-field (§28.3 borrow-lifetime rules). "A borrowed local
//   may not be assigned to any field of any object — neither owned nor non-
//   owned. Fields hold either owned references or runtime-managed references;
//   class-typed fields that hold a borrow are not expressible in this version."
//   Storing a borrow of an owned scope-bound local (directly, or via a closure
//   / borrow-bearing record carrying it) into a field lets the borrow outlive
//   the local, which is disposed at scope end — a use-after-dispose.
//
//   CASE 2 — out / ref / out-owned parameter (§28.3 borrow-lifetime rules).
//   An `out`/`ref` parameter location lives in the caller's frame, which
//   outlives this method's body. Writing a borrow of an owned scope-bound
//   local through such a parameter lets the borrow outlive its source — the
//   same lifetime violation as returning it. (§22.3: `out` produces a borrow
//   location for the caller; §28.3: a borrow may not outlive its source.)
//
//   CASE 3 — multi-hop alias chain. The owned source may be reached through
//   several intermediate locals before it escapes: `var b := r; var c := b;
//   return c;`. The borrow-source tracking is extended to follow the alias
//   chain transitively, so the eventual escape of `c` is recognized as an
//   escape of `r`.
//
// CASE 4 — a borrow whose SOURCE is a plain parameter — is deliberately NOT
// flagged. A parameter borrow's source lives in the CALLER's frame, which
// outlives this method; returning or otherwise escaping it within the caller
// is sound (the standard "borrow in, borrow out" pattern; §28.6 "Methods
// returning `this`"). The controls below pin that acceptance. See the report
// for the full §28.3 analysis of why this is correct rather than underspecified.
//
// SOUNDNESS DISCIPLINE: every positive case below rejects a program that the
// spec makes an error; every negative control is a structurally-similar but
// spec-legal program that must be accepted. Over-rejection is the cardinal sin
// for this milestone, so each positive is paired with several negatives that
// guard the exact boundary (move vs. borrow, param-source vs. local-source,
// value-typed copy vs. reference borrow, escaping vs. locally-used).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A resource class R with a destructor (a genuine non-trivial-dispose resource
// whose late read after dispose is the soundness hazard), an owning class
// `Box` with an owned reference field and a non-owned reference field, and the
// borrow-bearing record `Holder(Ref: R)`. `Box` is what an escape stores INTO.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public Inner: R? := null; "
        "  destructor end destructor; "
        "end R; "
        "record Holder(Ref: R); "
        "class Box "
        "  public Ref: R? := null; "         // non-`owned` (borrow) field — escape target
        "  public owned Owned: R? := null; " // owned field for the clean owned-store controls
        "end Box; "
        "class C ") + method + " end C;";
}

} // namespace

// =====================================================================
// CASE 1 — store-to-field of a borrow of an owned scope-bound local
//          (§28.3: a borrow may not be assigned to any field of any object).
// =====================================================================

// Direct: `box.Ref := r;` stores a re-borrow of the owned `using var` `r` into
// a field of `box`. The field outlives `r` (disposed at scope end), so the
// borrow dangles. (§28.3 borrow-lifetime rules.)
BEZEL_TEST(own_field_store_escape, store_reborrow_of_using_var_into_field_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(box: Box) "
        "  using var r := new R(); "
        "  box.Ref := r; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` stored into a field would outlive its source"));
}

// Direct, owned (non-`using var`) local source: same escape. `r` is owned by
// virtue of `new R()`; storing a borrow of it into a field outlives it.
BEZEL_TEST(own_field_store_escape, store_reborrow_of_owned_local_into_field_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(box: Box) "
        "  var r := new R(); "
        "  box.Ref := r; "
        "  r.Dispose(); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` stored into a field would outlive its source"));
}

// Store into `this.field` is the same: the field of the receiver outlives the
// local. `this.Held := r;` where `r` is an owned `using var` is an escape.
BEZEL_TEST(own_field_store_escape, store_reborrow_of_using_var_into_this_field_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; destructor end destructor; end R; "
        "class C "
        "  public Held: R? := null; "
        "  public method Leak() "
        "    using var r := new R(); "
        "    this.Held := r; "
        "  end Leak; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` stored into a field would outlive its source"));
}

// A borrow-bearing record carrying a borrow of an owned local, stored into a
// field. The record carries the borrow of `r`; storing the record into a field
// lets it outlive `r`. (§9.7 record borrow-lifetime + §28.3 field-store rule.)
BEZEL_TEST(own_field_store_escape, store_holder_record_borrowing_using_var_into_field_diagnosed_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(Ref: R); "
        "class Box public H: Holder := new Holder(null); end Box; "
        "class C "
        "  public static method Leak(box: Box) "
        "    using var r := new R(); "
        "    box.H := new Holder(r); "
        "  end Leak; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow-bearing record may not be stored") ||
        hasErrorMessage(r, "carries a borrow of `r`"));
}

// --- CASE 1 controls (must NOT be rejected) -------------------------------

// CONTROL (§28.8): assigning an owned local into an OWNED field by `move` is a
// transfer, not a borrow store. The field takes over the obligation; no borrow
// escapes. `box.Owned := move r;` is legal. (Targets the `owned` field — a
// `move` into a non-`owned` field would leak, #1020 / §28.6 v0.153.)
BEZEL_TEST(own_field_store_escape, store_move_of_owned_local_into_field_clean_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Give(box: Box) "
        "  var r := new R(); "
        "  box.Owned := move r; "
        "end Give;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.3): storing a borrow of a PARAMETER into a field is not flagged
// by this slice — the parameter's source lives in the caller's frame, not a
// scope-bound local. (The broader §28.3 "no borrow in any field" rule for
// param borrows is out of scope here; see the report.)
BEZEL_TEST(own_field_store_escape, store_param_borrow_into_field_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Wire(box: Box, src: R) "
        "  box.Ref := src; "
        "end Wire;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.3): a VALUE-TYPED projection of an owned local stored into a
// value-typed field is a copy, not a borrow. `box-of-int.N := r.V;` cannot
// dangle. (Guards the value-vs-borrow boundary.)
BEZEL_TEST(own_field_store_escape, store_value_projection_into_field_clean_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; destructor end destructor; end R; "
        "class IntBox public N: int := 0; end IntBox; "
        "class C "
        "  public static method Copy(box: IntBox) "
        "    using var r := new R(); "
        "    box.N := r.V; "
        "  end Copy; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.7): storing a freshly-`new`'d owned value into an OWNED field is
// the canonical owned-field assignment — `box.Owned := new R();` is legal (the
// field takes the obligation; the `new` result has no scope-bound source). The
// same store into a non-`owned` field would leak (#1020 / §28.6 v0.153).
BEZEL_TEST(own_field_store_escape, store_new_into_field_clean_28_7) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Init(box: Box) "
        "  box.Owned := new R(); "
        "end Init;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// CASE 2 — write a borrow of an owned scope-bound local through an
//          out / ref / out-owned parameter (§28.3: the parameter location
//          is the caller's frame, which outlives the local).
// =====================================================================

// `out o: R` then `o := r;` where `r` is an owned `using var`. The out-param
// location is in the caller; the borrow of `r` written through it outlives `r`.
BEZEL_TEST(own_outparam_escape, write_reborrow_of_using_var_through_out_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(out o: R) "
        "  using var r := new R(); "
        "  o := r; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` written through an out/ref parameter") ||
        hasErrorMessage(r, "§28.3 borrow-lifetime rule"));
}

// `ref o: R` then `o := r;` — a `ref` location is borrowed by the callee but
// lives in the caller's frame (§22.6: a `ref` location is borrowed by
// definition); writing a borrow of an owned local through it escapes.
BEZEL_TEST(own_outparam_escape, write_reborrow_of_owned_local_through_ref_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(ref o: R) "
        "  var r := new R(); "
        "  o := r; "
        "  r.Dispose(); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` written through an out/ref parameter") ||
        hasErrorMessage(r, "§28.3 borrow-lifetime rule"));
}

// A closure capturing an owned local written through an out-param of function
// type. The captured `r` is a borrow of the enclosing scope (§28.9); writing
// the closure through the out-param lets it outlive `r`.
BEZEL_TEST(own_outparam_escape, write_closure_capturing_using_var_through_out_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(out f: () -> int) "
        "  using var r := new R(); "
        "  f := lambda() => r.V; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` written through an out/ref parameter") ||
        hasErrorMessage(r, "§28.3 borrow-lifetime rule"));
}

// --- CASE 2 controls (must NOT be rejected) -------------------------------

// CONTROL (§28.8): `out owned o: R` written with a `move` of an owned local is
// a transfer into the out-owned slot, not a borrow escape. `o := move r;` is
// the legal out-owned production pattern.
BEZEL_TEST(own_outparam_escape, write_move_of_owned_local_through_out_owned_clean_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Produce(out owned o: R) "
        "  var r := new R(); "
        "  o := move r; "
        "end Produce;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.3 param-source): writing a borrow of a PARAMETER through an
// out-param is not flagged here — the parameter's source outlives this method.
BEZEL_TEST(own_outparam_escape, write_param_borrow_through_out_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Pass(src: R, out o: R) "
        "  o := src; "
        "end Pass;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: writing a VALUE-TYPED projection of an owned local through a
// value-typed out-param is a copy, not a borrow. `out n: int` with `n := r.V;`.
BEZEL_TEST(own_outparam_escape, write_value_projection_through_out_clean_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class R public V: int := 0; destructor end destructor; end R; "
        "class C "
        "  public static method Copy(out n: int) "
        "    using var r := new R(); "
        "    n := r.V; "
        "  end Copy; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// CASE 3 — multi-hop alias chain to an owned scope-bound local.
// =====================================================================

// `var b := r; var c := b; return c;` — `c` aliases `b` which aliases the
// owned `using var` `r`. Following the chain, returning `c` escapes `r`.
BEZEL_TEST(own_multihop_escape, return_two_hop_reborrow_of_using_var_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(): R "
        "  using var r := new R(); "
        "  var b := r; "
        "  var c := b; "
        "  return c; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "§28.3 / §9.7 borrow-lifetime rule"));
}

// Three-hop chain, exercising the transitive follow more than once.
BEZEL_TEST(own_multihop_escape, return_three_hop_reborrow_of_using_var_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(): R "
        "  using var r := new R(); "
        "  var b := r; "
        "  var c := b; "
        "  var d := c; "
        "  return d; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "§28.3 / §9.7 borrow-lifetime rule"));
}

// Multi-hop where the FINAL escape is a store-to-field rather than a return:
// `var b := r; box.Ref := b;` stores the two-hop borrow of `r` into a field.
BEZEL_TEST(own_multihop_escape, store_two_hop_reborrow_into_field_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Leak(box: Box) "
        "  using var r := new R(); "
        "  var b := r; "
        "  box.Ref := b; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `r` stored into a field"));
}

// --- CASE 3 controls (must NOT be rejected) -------------------------------

// v0.180 (§28.3 Pick-prohibition, #1547): a STATIC method returning a
// parameter-sourced borrow (even multi-hop) is REJECTED. A static method has no
// receiver source, and the caller-side sourceless-return owned-assumption (§28.3
// "Factory methods") is sound only if the callee-side rule rejects EVERY borrow
// return. Supersedes the pre-v0.180 "deliberately not flagged" behavior.
BEZEL_TEST(own_multihop_escape, return_two_hop_reborrow_of_param_rejected_28_3_v0180) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Pass(src: R): R "
        "  var b := src; "
        "  var c := b; "
        "  return c; "
        "end Pass;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned from a static method"));
}

// CONTROL: a multi-hop chain that is used LOCALLY (a value projection read)
// and never escapes is accepted. `r` outlives every read; it is disposed
// normally at scope end.
BEZEL_TEST(own_multihop_escape, two_hop_reborrow_used_locally_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Use(): int "
        "  using var r := new R(); "
        "  var b := r; "
        "  var c := b; "
        "  return c.V; "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a value-typed hop breaks the borrow chain. `var n := r.V;` (int)
// is a copy; `var m := n; return m;` returns a value, not a borrow of `r`.
BEZEL_TEST(own_multihop_escape, value_typed_hop_breaks_chain_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(): int "
        "  using var r := new R(); "
        "  var n := r.V; "
        "  var m := n; "
        "  return m; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a multi-hop chain whose tail is rebound to null before escape does
// not carry a live borrow. `var b := r; var c: R? := b; c := null; return c;`.
// v0.201 (#2048): the static factory's class-typed return is spelled `owned R?`
// per the declaration-site static-return rule.
BEZEL_TEST(own_multihop_escape, multihop_rebound_to_null_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(): owned R? "
        "  using var r := new R(); "
        "  var b := r; "
        "  var c: R? := b; "
        "  c := null; "
        "  return c; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// CASE 4 — a STATIC method returning a parameter-sourced borrow is REJECTED
// (v0.180, §28.3 Pick-prohibition, #1547). A static method has no receiver, so
// the caller treats a sourceless non-`owned` return as OWNED; that is sound only
// because the callee-side rule rejects a static method returning a borrow of one
// of its parameters (otherwise the borrow is misclassified as owned -> double free).
// [Superseded: pre-v0.180 these parameter-source returns were "not flagged".]
// A record/closure/`this` return is a DIFFERENT rule and stays accepted (below).
// =====================================================================

// v0.180: returning a direct re-borrow of a parameter from a static method is REJECTED.
BEZEL_TEST(own_param_source, return_reborrow_of_param_rejected_28_3_v0180) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Pass(src: R): R "
        "  var b := src; "
        "  return b; "
        "end Pass;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned from a static method"));
}

// CONTROL: returning a record borrowing a parameter is accepted (parallels the
// existing return_record_borrowing_param_clean direct control).
BEZEL_TEST(own_param_source, return_holder_borrowing_param_clean_9_7) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Wrap(src: R): Holder "
        "  var h := new Holder(src); "
        "  return h; "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: returning a closure capturing a parameter is accepted — the
// captured parameter's source is the caller's frame, which outlives the
// returned closure.
BEZEL_TEST(own_param_source, return_closure_capturing_param_clean_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Make(src: R): () -> int "
        "  return lambda() => src.V; "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

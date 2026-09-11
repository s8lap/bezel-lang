// Spec-derived tests for fix #4283 — explicit `.Dispose()` through an ALIAS
// of a still-owned FIELD (`this._arr`, a bare `_arr`, a nested field chain,
// a field read through a parameter, or a static field) double-frees, with no
// compile-time diagnostic. Adjacent to, but NOT introduced by, fix #4236
// (§28.3/§28.6, v0.298) and fix #4268 (§28.10, v0.298): both fixed the
// IDENTICAL alias-dispose double-free for a LOCAL/PARAMETER root only — the
// field axis was never in either fix's scope (found while sweeping #4268's
// own axis grid, filed as issue #4283, fixed here).
//
// §28.6 "Leak prevention" (v0.298): "A bare read of an owned field lends
// (K9.1) and may be returned through a non-`owned` return type as a borrow
// of the field ... The caller's borrow carries the ordinary K2.3
// obligations — no disposal (the receiver rule below, [Who may call
// `Dispose()`])" — a bare/qualified read of an OWNED field is itself
// classified as a lending borrow, the same K3.13 no-silent-consume
// principle §28.3 "Local variables, ownership, and borrows" applies to an
// unmarked read of a named owned LOCAL binding (e.g. the `??`/if-expression
// arm rows: "An unmarked read of a named owned binding remains a borrow
// arm (K3.13 — no silent consume)"). So `var b := this._arr;` (no `move`)
// makes `b` a BORROW of the field's current value. §28.6's own "Who may
// call `Dispose()`" subsection then settles the receiver question directly:
// its admission list names only "an owned local or `owned` parameter, a
// `using var`, an unbound owned temporary (§28.3), a `move` result, or an
// `owned` field / owning-collection slot / `static owned` field read by
// code of the owning object's declaring type" as legal receivers, and its
// borrow-receivers list separately states "`Dispose()` through a borrow is
// a compile error. The borrow receivers: a borrowed local or parameter" —
// `b` is exactly that: a borrowed LOCAL, regardless of what field it was
// read from, so `b.Dispose()` must be rejected exactly as an aliased
// local/parameter already is (fix #4236/#4268). Ownership.cpp's
// checkDisposeReceiverOwnership already enforces the field-read half of
// this directly for a DIRECT field-read receiver (`this._arr.Dispose();`,
// admitted only via `exprIsReceiverOwnedFieldBorrow` — the §28.7 "field
// admission" carve-out for "the dispose-then-assign legacy and the
// destructor idioms", #1230); this fix closes the gap for the ALIASED
// (through-a-local) receiver shape that check does not cover.
//
// THE GAP: `disposeAliasSource`'s population at a LocalDecl/LocalConstDecl
// only ever recognized a bare-IDENTIFIER initializer whose resolved symbol
// passed `isOwnedTrackedLocal` — a predicate that reads `states_`, which a
// FieldSymbol never occupies (fields are object state, not scope state, per
// §K9). That is true for EVERY field read, not only a `MemberAccessExpr`
// (`this._arr`) — a BARE field read (`_arr`, implicit `this`) is also an
// `IdentifierExpr` that resolves to a FieldSymbol, and `isOwnedTrackedLocal`
// answers false for it just the same. So no field-rooted alias was ever
// tracked, regardless of spelling.
//
// THE FIX: both declaration sites (`handleLocalDecl`'s plain-`var`/`using
// var` arm, and `visitStmt`'s `LocalConstDecl` arm) now ALSO populate
// `disposeAliasSource` when the initializer is `exprIsOwnedFieldRead` (§28.3,
// issue #398) — the SAME authority `checkDisposeReceiverOwnership`'s own
// direct field-read arm already uses, reused rather than re-derived
// (LANE_RULES §6/§25). It answers purely from the LEAF field's `owned`
// modifier, treating a bare identifier and any `recv.f`/`this.f.g` chain of
// any depth identically, so it needs no `states_`-style liveness tracking.
//
// SCOPE (LANE_RULES §6, deliberate): only the two DECLARATION sites are
// widened. `handleAssignment` and the lambda-body reassignment arm are left
// untouched — commit f1b849b (issue 4272, this same batch) added
// reassignment-axis rows (`y := h.item; y.Dispose();`) that deliberately
// pin the CURRENT accept-and-runtime-protected reassignment behavior;
// widening the reassignment sites too would flip those rows to a
// compile-time rejection.
// The four-site consolidation this suggests is filed as a follow-up (see the
// commit body for the issue URL) rather than attempted here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT arm — bare field read (implicit `this`) and explicit `this.field`,
// both spellings (`var`/`const` and `using var`).
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           bare_field_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    var b: Array<int> := _arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           this_field_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    var b: Array<int> := this._arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           this_field_alias_nullcond_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    var b: Array<int> := this._arr; b?.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

// `const` — the parallel `LocalConstDecl` arm (same send-back shape #4236
// itself needed for `const`, LANE_RULES §6's sibling trap, one axis over).
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           const_field_alias_dot_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    const b: Array<int> := _arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

// =====================================================================
// REJECT arm — `using var` spelling of the same two shapes.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           using_var_bare_field_alias_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    using var b: Array<int> := _arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           using_var_this_field_alias_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method BadDispose(): int "
        "    using var b: Array<int> := this._arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

// =====================================================================
// REJECT arm — a NESTED field path (`this._a._b`): the leaf field, not the
// intermediate one, decides the classification, at any chain depth.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           nested_field_path_alias_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; "
        "class Inner "
        "  public owned _b: Array<int>; "
        "  public constructor() _b := new Array<int>(2); end constructor; "
        "end Inner; "
        "class Holder "
        "  private owned _a: Inner; "
        "  public constructor() _a := new Inner(); end constructor; "
        "  public method BadDispose(): int "
        "    var b: Array<int> := this._a._b; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_b`"));
}

// =====================================================================
// REJECT arm — a field read through a PARAMETER (not `this`): the receiver
// identity is irrelevant to `exprIsOwnedFieldRead`, only the leaf field's
// `owned` modifier is.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           field_alias_via_parameter_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  public owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "end Holder; "
        "class C "
        "  public static method BadDispose(h: Holder): int "
        "    var b: Array<int> := h._arr; b.Dispose(); return 0; "
        "  end BadDispose; "
        "  public static method Main(): int "
        "    using var h := new Holder(); return BadDispose(h); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

// =====================================================================
// REJECT arm — a STATIC field. §22.12/§22.13: a static `owned` field is
// always `readonly`, program-owned, disposed at process exit
// (`bz_dispose_static_owned`) — an alias-dispose is the exact double-free
// #2021's move-based sibling already guards against move.
// =====================================================================

BEZEL_TEST(ownership_dispose_aliased_field_4283,
           static_field_alias_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Registry "
        "  public static owned readonly Slot: Array<int> := new Array<int>(); "
        "  public static method BadDispose(): int "
        "    var b: Array<int> := Slot; b.Dispose(); return 0; "
        "  end BadDispose; "
        "end Registry; "
        "class C public static method Main(): int "
        "  return Registry.BadDispose(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `Slot`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling;
// a false-reject here would be worse than #4283 itself.
// =====================================================================

// `using var` over a genuine `new` — a NewExpr initializer, never matching
// either the identifier or field-read population, by construction.
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           using_var_fresh_producer_still_compiles_28_10) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var b := new Array<int>(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A call-result `using var` (a CallExpr initializer, never a MemberAccessExpr
// or IdentifierExpr field read) must keep compiling AND disposing cleanly.
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           using_var_call_result_still_compiles_28_10) {
    auto r = runAot(
        "module M; "
        "class Helper public static method Make(): owned Array<int> "
        "  var x := new Array<int>(2); x.Add(1); return x; "
        "end Make; end Helper; "
        "class C public static method Main(): int "
        "  using var b := Helper.Make(); b.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A field alias that is declared but NEVER explicitly disposed must still
// compile (this check only ever fires AT a `.Dispose()`/`?.Dispose()` call
// site) and the object's own scope-end auto-drop must be the only free, every
// iteration — no leak, no double-free from merely SETTING
// `disposeAliasSource` (codegen never consults it; only the dispose-call-site
// check does). Loop-amplified per LANE_RULES §3/§30 (a one-shot LSan row can
// pass on both sides of a fix).
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           field_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); _arr.Add(1); "
        "  end constructor; "
        "  public method Peek(): int var b: Array<int> := _arr; return b.Count; "
        "  end Peek; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(); var n := h.Peek(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The identical never-disposed control spelled `using var` — per §28.10 the
// field-rooted `using var` alias does not itself adopt a disposal obligation
// either (it is a borrow, pinned only), so the object's own auto-drop is
// still the sole free.
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           using_var_field_alias_never_disposed_frees_exactly_once_28_10) {
    auto r = runAotChecked(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); _arr.Add(1); "
        "  end constructor; "
        "  public method Peek(): int "
        "    using var b: Array<int> := _arr; return b.Count; "
        "  end Peek; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(); var n := h.Peek(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The DIRECT dispose-then-assign idiom (§28.7/#1230) — disposing the field
// itself, through `this`, with no intervening alias — is a completely
// different code path (`exprIsReceiverOwnedFieldBorrow`, consulted only at
// the `.Dispose()` receiver check) and this fix never touches it. Must keep
// compiling and running leak/UAF-free. Loop-amplified.
BEZEL_TEST(ownership_dispose_aliased_field_4283,
           direct_field_dispose_then_assign_idiom_still_compiles_28_7) {
    auto r = runAotChecked(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method Replace(): int "
        "    this._arr.Dispose(); _arr := new Array<int>(3); return 0; "
        "  end Replace; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var i := 0; "
        "  while i < 200 do var r := h.Replace(); i := i + r + 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

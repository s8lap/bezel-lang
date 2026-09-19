// Spec-derived tests for issue #360 — the direct-return escape forms of two
// ownership soundness holes:
//
//   D1 (§28.9): returning a closure that captures an owned (`using var` /
//   owned) method-scope local. "Capturing an *owned* class-typed local does
//   **not** transfer ownership into the closure. Instead, the capture is
//   treated as a borrow whose source is the local's enclosing scope; the
//   closure may not outlive that scope." A returned closure outlives that
//   scope, so the captured local — disposed at scope exit — is read after
//   dispose. This must be a compile error.
//
//   D2 (§9.7): returning a borrow-bearing record that carries a borrow of an
//   owned (`using var` / owned) method-scope local. "A borrow-bearing record
//   value may not outlive the source of any borrow it contains" and "may not
//   be returned from a method whose return-type rules cannot establish a
//   sufficient lifetime for every borrow the record carries." Returning it is
//   exactly such an escape.
//
// The controls below guard against over-rejection: capturing a value-typed
// local, capturing an owned local in a closure that does NOT escape, returning
// a value (not a borrow) computed from a captured owned local, and a
// non-borrow-bearing / null-source record are all valid and must be accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A class R with a destructor — a genuine non-trivial-dispose resource whose
// late read after dispose is the soundness hazard. Wrapped around the method
// under test.
std::string wrapClosure(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end R; "
        "class C ") + method + " end C;";
}

std::string wrapRecord(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end R; "
        "record Holder(Ref: R); "
        "record Point(X: int); "
        "class C ") + method + " end C;";
}

} // namespace

// --- D1: §28.3 returned-closure-capture escape (REJECT) -------------------

// The escape_closure.bzl reproducer: a closure capturing a `using var` owned
// local is returned; the local is disposed at scope end, the closure reads it
// after dispose. (§28.3: an owned capture is a borrow of the enclosing scope;
// the closure may not outlive that scope.)
BEZEL_TEST(own_closure_escape, return_lambda_capturing_using_var_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeEscaping(): () -> int "
        "  using var r := new R(); "
        "  r.V := 7; "
        "  return lambda() => r.V; "
        "end MakeEscaping;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r`"));
}

// An owned local that is NOT a `using var` but is owned by virtue of holding a
// `new` result carries the same scope-bound discharge obligation; capturing it
// in a returned closure is the same §28.9 escape.
BEZEL_TEST(own_closure_escape, return_lambda_capturing_owned_local_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeEscaping(): () -> int "
        "  var r := new R(); "
        "  return lambda() => r.V; "
        "end MakeEscaping;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r`"));
}

// --- D1 controls: closures that do NOT escape an owned source (ACCEPT) ----

// CONTROL (§28.9 non-escape): a closure capturing a NON-owned (value-typed)
// local. A captured `int` is copied into the closure and carries no borrow
// lifetime — returning the closure is valid.
BEZEL_TEST(own_closure_escape, return_lambda_capturing_value_local_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeValue(): () -> int "
        "  var n := 5; "
        "  return lambda() => n; "
        "end MakeValue;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.9 non-escape): a closure capturing an owned `using var` local
// that does NOT escape — it is invoked locally (while the source is alive) and
// only its int result leaves the method. No closure outlives the scope.
BEZEL_TEST(own_closure_escape, capture_owned_local_invoked_locally_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method UseLocally(): int "
        "  using var r := new R(); "
        "  var f := lambda() => r.V; "
        "  return f(); "
        "end UseLocally;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.9 non-escape): returning a VALUE (not a borrow, not the
// closure) computed from a captured owned local. The lambda is invoked inline
// while the source is alive; the returned `int` is a copy. No closure escapes.
BEZEL_TEST(own_closure_escape, return_value_from_captured_owned_local_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method ReturnValue(): int "
        "  using var r := new R(); "
        "  return (lambda() => r.V)(); "
        "end ReturnValue;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.9 non-escape): a returned closure that captures NOTHING from
// the enclosing scope (a constant lambda) is always valid.
BEZEL_TEST(own_closure_escape, return_non_capturing_lambda_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Const(): () -> int "
        "  return lambda() => 42; "
        "end Const;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- D2: §9.7 returned borrow-bearing record escape (REJECT) --------------

// The escape_record.bzl reproducer: a borrow-bearing record `Holder(Ref: R)`
// is built from a `using var` owned local and returned. The record carries a
// borrow of `r`, whose source is disposed at scope end. (§9.7: a borrow-
// bearing record may not outlive the source of any borrow it carries, and may
// not be returned from a method that cannot establish a sufficient lifetime.)
BEZEL_TEST(own_record_escape, return_record_borrowing_using_var_diagnosed_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Leak(): Holder "
        "  using var r := new R(); "
        "  r.V := 5; "
        "  return new Holder(r); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of `r`"));
}

// §9.7 with an owned (non-`using var`) local source: same escape.
BEZEL_TEST(own_record_escape, return_record_borrowing_owned_local_diagnosed_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Leak(): Holder "
        "  var r := new R(); "
        "  return new Holder(r); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of `r`"));
}

// --- D2 controls: records that carry no escaping borrow (ACCEPT) ----------

// CONTROL (§9.7): a NON-borrow-bearing record (only a value-typed `int` field)
// carries no borrow, so returning it — even built from local data — is valid.
BEZEL_TEST(own_record_escape, return_value_record_clean_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method MakePoint(): Point "
        "  var n := 3; "
        "  return new Point(n); "
        "end MakePoint;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§9.7 null-source form): a borrow-bearing record whose borrow-
// carrying field is `null` carries no live borrow source; returning it is the
// null-source exception the spec explicitly admits.
//
// The borrow field must be NULLABLE for the null-source form to be expressible:
// per §9.7 (L979) the form is "every reference-typed field is null", which a
// non-nullable `R` field can never satisfy (§7 forbids null in a non-nullable
// reference). Migrated for #1105: `new Record(null)` is now type-checked, and
// `null` into the shared `Holder(Ref: R)` (non-nullable) is a §7 type error
// (exactly as `var x: R := null` / `Take(null)` are) — so this test uses a
// record with a nullable borrow field, which is the genuine null-source form.
BEZEL_TEST(own_record_escape, return_record_null_source_clean_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record NHolder(Ref: R?); "
        "class C "
        "  public static method None(): NHolder "
        "    return new NHolder(null); "
        "  end None; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #4428 (§9.7, v0.301): a borrow-bearing record built from a *parameter*
// borrow is REJECTED at the return site. The §9.7 v0.301 ruling closes the
// parameter-sourced arm of the borrow-return admission set — "a record type
// announces no source set at all, so a caller has nothing but the receiver to
// re-establish and a parameter-sourced record return would hand back a borrow
// no call site could bound" (spec §9.7 v0.301). The pre-fix control pinned
// the opposite verdict (BEZEL_EXPECT_FALSE hasErrors), which was the
// soundness hole #4428 — its comment "consistent with the existing param-
// borrow treatment" conflated the §28.3 caller-side rule (a parameter-source
// is fine to bind at the call site) with the §9.7 callee-side rule (a
// parameter-source cannot be the ADMITTED return arm of a borrow-bearing
// record). The two rules operate at different sides and the call-site shape
// can re-establish a parameter source only when the declaration announces it
// — which a record type does not. Renamed and re-polarized here. // per
// §9.7 v0.301; the callee-side rejection is reportIfParameterRoot.
BEZEL_TEST(own_record_escape, return_record_borrowing_param_diagnosed_9_7_v301) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Wrap(c: R): Holder "
        "  return new Holder(c); "
        "end Wrap;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `c`"));
}

// =====================================================================
// SECOND SLICE (issue #360): INDIRECT escapes via an aliasing local, and
// borrowed-reference local captures. The escape is bound into a local one
// statement earlier, then that local is returned. The owned source's borrow
// would outlive the source, exactly as the direct forms above.
// =====================================================================

// A class with a reference field, so a re-borrow of a *reference* field of an
// owned local (`var b := r.Inner;`) can be exercised distinctly from the
// value-typed `r.V` projection.
namespace {
std::string wrapRefField(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public Inner: R? := null; "
        "  destructor end destructor; "
        "end R; "
        "record Holder(Ref: R); "
        "class C ") + method + " end C;";
}
} // namespace

// --- Indirect re-borrow of an owned local then return (REJECT) ------------

// `var b := r; return b;` — `b` is a borrow of the owned `using var` `r`
// (§28.3: reading an owned local in a borrow-consuming position is a borrow,
// source `r`). Returning `b` lets that borrow outlive `r`, disposed at scope
// end. (§28.3 borrow-lifetime rule, reached indirectly through the local.)
BEZEL_TEST(own_indirect_escape, return_reborrow_of_using_var_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Leak(): R "
        "  using var r := new R(); "
        "  var b := r; "
        "  return b; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `var b := r.Inner; return b;` — a re-borrow of a *reference* field of an
// owned local. The field access borrows `r` (§28.3: `obj.f` is a borrow,
// source `obj`); returning `b` outlives `r`.
BEZEL_TEST(own_indirect_escape, return_reborrow_of_reference_field_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrapRefField(
        "public static method Leak(): R? "
        "  using var r := new R(); "
        "  var b := r.Inner; "
        "  return b; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `var h := new Holder(r); return h;` — a borrow-bearing record bound to a
// holder local, then returned. The record carries a borrow of the owned `r`
// (§9.7); returning the holder lets it outlive `r`.
BEZEL_TEST(own_indirect_escape, return_holder_record_aliasing_using_var_diagnosed_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Leak(): Holder "
        "  using var r := new R(); "
        "  var h := new Holder(r); "
        "  return h; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `var f := () => r.V; return f;` — a closure capturing the owned `r` bound
// to a holder local, then returned. Capturing an owned local is a borrow of
// its enclosing scope (§28.9); returning the closure lets it outlive `r`.
BEZEL_TEST(own_indirect_escape, return_closure_holder_capturing_using_var_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Leak(): () -> int "
        "  using var r := new R(); "
        "  var f := lambda() => r.V; "
        "  return f; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The borrow source can also be bound by a later ASSIGNMENT (not only the
// declaration initializer): `var b: R? := null; b := r; return b;`. This
// exercises the handleAssignment source-population path. The source `r` is a
// `using var`, so the ONLY error here is the escape — making this test load-
// bearing on the indirect-escape check (a `using var` has no independent leak
// diagnostic to mask it).
BEZEL_TEST(own_indirect_escape, return_reborrow_bound_by_assignment_diagnosed_28_3) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Leak(): R? "
        "  using var r := new R(); "
        "  var b: R? := null; "
        "  b := r; "
        "  return b; "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Indirect controls: aliasing locals that do NOT escape (ACCEPT) -------

// CONTROL: a VALUE-TYPED re-borrow (`var n := r.V;` where V is int) is a
// copy, not a borrow of `r` (§28.3 value-typed returns). Returning it can
// never outlive `r`, so it must be accepted. This is the key false-positive
// guard for the soundness gate.
BEZEL_TEST(own_indirect_escape, return_value_reborrow_clean_28_3) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Take(): int "
        "  using var r := new R(); "
        "  var n := r.V; "
        "  return n; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a holder record built from a *parameter* borrow (source: caller's
// frame, outlives this method) bound to a local and returned is accepted —
// the parameter is not an owned scope-bound local. (Parallels the direct
// param-borrow control.)
BEZEL_TEST(own_indirect_escape, return_holder_aliasing_param_clean_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Wrap(c: R): Holder "
        "  var h := new Holder(c); "
        "  return h; "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: an indirect holder that is built but does NOT escape — it is used
// locally (a value field read) and never returned. `r` is disposed normally
// at scope end; no borrow outlives it.
BEZEL_TEST(own_indirect_escape, holder_aliasing_using_var_not_returned_clean_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Use(): int "
        "  using var r := new R(); "
        "  var h := new Holder(r); "
        "  return h.Ref.V; "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a borrow used locally then the source disposed — `var b := r;`
// where `r` is a plain owned `var` explicitly disposed before return; `b` is
// never returned, only its value field is read while `r` is still alive.
BEZEL_TEST(own_indirect_escape, reborrow_used_locally_then_source_disposed_clean_28_3) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Use(): int "
        "  var src := new R(); "
        "  var b := src; "
        "  var n := b.V; "
        "  src.Dispose(); "
        "  return n; "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a closure holder that is INVOKED locally (not returned) — its int
// result leaves the method, the closure does not. `r` outlives every read.
BEZEL_TEST(own_indirect_escape, closure_holder_invoked_locally_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Use(): int "
        "  using var r := new R(); "
        "  var f := lambda() => r.V; "
        "  return f(); "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: rebinding the holder to a non-borrowing value before return clears
// the tracked borrow source — `var b := r; b := null; return b;` does not
// escape `r` (the returned value is null, no live borrow). Guards against a
// stale-source false positive across reassignment. v0.201 (#2048): the static
// factory's class-typed return is spelled `owned R?` per the declaration-site
// static-return rule.
BEZEL_TEST(own_indirect_escape, reborrow_then_rebound_to_null_clean_28_3) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Take(): owned R? "
        "  using var r := new R(); "
        "  var b: R? := r; "
        "  b := null; "
        "  return b; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

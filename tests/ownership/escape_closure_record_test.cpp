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

// --- D1: §28.9 returned-closure-capture escape (REJECT) -------------------

// The escape_closure.bzl reproducer: a closure capturing a `using var` owned
// local is returned; the local is disposed at scope end, the closure reads it
// after dispose. (§28.9: an owned capture is a borrow of the enclosing scope;
// the closure may not outlive that scope.)
BEZEL_TEST(own_closure_escape, return_lambda_capturing_using_var_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeEscaping(): () -> int "
        "  using var r := new R(); "
        "  r.V := 7; "
        "  return () => r.V; "
        "end MakeEscaping;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An owned local that is NOT a `using var` but is owned by virtue of holding a
// `new` result carries the same scope-bound discharge obligation; capturing it
// in a returned closure is the same §28.9 escape.
BEZEL_TEST(own_closure_escape, return_lambda_capturing_owned_local_diagnosed_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeEscaping(): () -> int "
        "  var r := new R(); "
        "  return () => r.V; "
        "end MakeEscaping;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- D1 controls: closures that do NOT escape an owned source (ACCEPT) ----

// CONTROL (§28.9 non-escape): a closure capturing a NON-owned (value-typed)
// local. A captured `int` is copied into the closure and carries no borrow
// lifetime — returning the closure is valid.
BEZEL_TEST(own_closure_escape, return_lambda_capturing_value_local_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method MakeValue(): () -> int "
        "  var n := 5; "
        "  return () => n; "
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
        "  var f := () => r.V; "
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
        "  return (() => r.V)(); "
        "end ReturnValue;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.9 non-escape): a returned closure that captures NOTHING from
// the enclosing scope (a constant lambda) is always valid.
BEZEL_TEST(own_closure_escape, return_non_capturing_lambda_clean_28_9) {
    auto r = parseAndCheckOwnership(wrapClosure(
        "public static method Const(): () -> int "
        "  return () => 42; "
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
}

// §9.7 with an owned (non-`using var`) local source: same escape.
BEZEL_TEST(own_record_escape, return_record_borrowing_owned_local_diagnosed_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Leak(): Holder "
        "  var r := new R(); "
        "  return new Holder(r); "
        "end Leak;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
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
BEZEL_TEST(own_record_escape, return_record_null_source_clean_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method None(): Holder "
        "  return new Holder(null); "
        "end None;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§9.7): a borrow-bearing record built from a *parameter* borrow
// (whose source is the caller's frame, not a scope-bound local) is accepted —
// consistent with the existing param-borrow treatment (return_member_call_on_
// param_clean). Only an owned scope-bound local triggers the §9.7 rejection in
// this slice.
BEZEL_TEST(own_record_escape, return_record_borrowing_param_clean_9_7) {
    auto r = parseAndCheckOwnership(wrapRecord(
        "public static method Wrap(c: R): Holder "
        "  return new Holder(c); "
        "end Wrap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

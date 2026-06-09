// Spec-derived tests for issue #530 — `for var e in dict` on a
// Dictionary<K,V> or OwnedDictionary<K,V> must emit a CLEAR diagnostic
// explaining that direct iteration is not supported (stdlib §4.8 / §4.9).
//
// Per stdlib §4.8: Dictionary<K,V> has NO default `for var e in d` iteration;
// the idiomatic patterns are `for var k in d.Keys` (key-iteration) or
// `d.Drain()` (OwnedDictionary only). Per §4.9: OwnedDictionary<K,V> also
// provides no default direct iteration.
//
// Before fix (#530): `for var e in d do var v: int := e.Value; end;` emitted
//   "string member `.Value` is not yet supported in codegen"
// because sema mis-typed the element variable as K (the first generic arg),
// and the failure surfaced late at `.Value`.
//
// After fix: sema emits the §4.8-cited diagnostic at the for-statement
// (element type is NOT inferred; the loop body is still type-checked as a
// fallback). Tests assert:
//   (a) hasErrors() is true
//   (b) some diagnostic message contains "not directly iterable" or "Keys"
//       AND contains "§4.8" (i.e. the new, clear message)
//   (c) the OLD confusing message "not yet supported in codegen" is ABSENT.
//
// Control tests verify that the guard does NOT break:
//   - `for var k in d.Keys` (Dictionary's Keys is ICollection<K> — iterable)
//   - `for var x in arr` (Array<T>)
//   - `for var x in someSet` (Set<T>)
//   - `for var i in 0..5` (range)

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Returns true iff at least one diagnostic message contains `needle`.
bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// Returns true iff NO diagnostic contains `needle` (used to verify the old
// confusing message is absent).
bool noDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    return !anyDiagContains(r, needle);
}

// Helper: wrap body in a module/class/method that has
//   d: Dictionary<string,int>
//   od: OwnedDictionary<string,int>
//   arr: Array<int>
//   s: Set<int>
//   n: int
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class C "
        "public method M("
        "  d: Dictionary<string, int>, "
        "  od: OwnedDictionary<string, int>, "
        "  arr: Array<int>, "
        "  s: Set<int>, "
        "  n: int"
        ") ") + body + " end M; end C;";
}

} // namespace

// ===========================================================================
// §4.8 — Dictionary<K,V> direct iteration must be rejected with clear message
// ===========================================================================

// §4.8: `for var e in d` on a Dictionary<string,int> parameter must emit an
// error. The old confusing "not yet supported in codegen" message must be
// absent; the new message must mention "not directly iterable" or "Keys" plus
// the §4.8 citation (so it is actionable).
// (Spec basis: stdlib §4.8 — "default `for var entry in dict` iteration is
// not provided; use key-iteration or Drain()".)
BEZEL_TEST(sema_dict_iteration_530, dict_direct_iteration_is_rejected_spec_4_8) {
    auto r = parseSemaAll(wrap("for var e in d do n := n + 1; end;"));
    // Must produce an error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The new clear message must cite §4.8 and hint at `.Keys`.
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "\xC2\xA7""4.8"));   // "§4.8" in UTF-8
    // The old confusing codegen-surfaced message must be gone.
    BEZEL_EXPECT_TRUE(noDiagContains(r, "not yet supported in codegen"));
}

// §4.8: same Dictionary<string,int> iteration with body that accesses .Value
// (this was the specific scenario that produced the confusing message before
// the fix — sema typed `e` as `string` (K) and `.Value` failed late).
BEZEL_TEST(sema_dict_iteration_530, dict_direct_iteration_with_value_access_spec_4_8) {
    auto r = parseSemaAll(wrap("for var e in d do var v: int := e.Value; end;"));
    // Must produce an error — the §4.8 early diagnostic, not a late codegen one.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "\xC2\xA7""4.8"));
    BEZEL_EXPECT_TRUE(noDiagContains(r, "not yet supported in codegen"));
}

// §4.8: the diagnostic message must mention "Keys" to give the user the
// idiomatic fix pattern (`for var k in d.Keys do ... end`).
BEZEL_TEST(sema_dict_iteration_530, dict_diag_mentions_keys_spec_4_8) {
    auto r = parseSemaAll(wrap("for var e in d do n := n + 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "Keys"));
}

// ===========================================================================
// §4.9 — OwnedDictionary<K,V> direct iteration must also be rejected
// ===========================================================================

// §4.9: `for var e in od` on an OwnedDictionary<string,int> parameter must
// be rejected with the same clear §4.8/§4.9 diagnostic (the spec prose at
// §4.9 says "Default `for var entry in dict do` iteration is not provided").
BEZEL_TEST(sema_dict_iteration_530, owned_dict_direct_iteration_rejected_spec_4_9) {
    auto r = parseSemaAll(wrap("for var e in od do n := n + 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "\xC2\xA7""4.8"));
    BEZEL_EXPECT_TRUE(noDiagContains(r, "not yet supported in codegen"));
}

// ===========================================================================
// CONTROL — the guard must NOT break these existing-valid iteration patterns
// ===========================================================================

// §4.8 / §4.9: `for var k in d.Keys` — Keys is ICollection<K>, which is
// iterable. The guard on Dictionary must not affect this pattern.
BEZEL_TEST(sema_dict_iteration_530, dict_keys_iteration_accepted_spec_4_8) {
    auto r = parseSemaAll(wrap("for var k in d.Keys do n := n + 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.3 / §29.2: `for var x in arr` over an Array<int> parameter.
// The generic-arg guard must leave Array<T> completely unaffected.
BEZEL_TEST(sema_dict_iteration_530, array_iteration_unaffected_spec_4_3) {
    auto r = parseSemaAll(wrap("for var x in arr do n := n + x; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.10 / §29.2: `for var x in s` over a Set<int> parameter.
// Set is iterable (implements IEnumerable<T>); the guard must not reject it.
BEZEL_TEST(sema_dict_iteration_530, set_iteration_unaffected_spec_4_10) {
    auto r = parseSemaAll(wrap("for var x in s do n := n + x; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.5 / §29.2: `for var i in 0..5` — an integer range literal.
// Ranges use a completely separate inference path (RangeExpr); the guard
// must not interfere.
BEZEL_TEST(sema_dict_iteration_530, range_iteration_unaffected_spec_16_5) {
    auto r = parseSemaAll(wrap("for var i in 0..5 do n := n + i; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

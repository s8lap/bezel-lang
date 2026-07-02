// Spec-derived tests for issue #530 / #548 — `for var e in dict` iteration
// on Dictionary<K,V> and OwnedDictionary<K,V>.
//
// v0.134 (#530): `for var e in d` on BOTH Dictionary and OwnedDictionary
// emitted a clear §4.8 diagnostic (direct iteration not provided).
//
// v0.135 (#548): the restriction is NARROWED to OwnedDictionary<K,V> only.
// Dictionary<K,V> now supports `for var e in d`, yielding
// DictionaryEntry<K,V> (stdlib §4.8 v0.135).  OwnedDictionary still rejects
// direct iteration (stdlib §4.9 v0.135).
//
// These tests are updated to reflect the v0.135 widening.  The sema tests
// that previously checked Dictionary rejection now verify ACCEPTANCE (no error,
// entry.Key/entry.Value type correctly).  OwnedDictionary rejection is
// unchanged.  Control tests remain unaffected.

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
//   od: OwnedDictionary<string,V>  (V is a class; stdlib §4.2/#1327 rejects int)
//   arr: Array<int>
//   s: Set<int>
//   n: int
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class V end V; "           // minimal class for OwnedDictionary value
        "class C "
        "public method M("
        "  d: Dictionary<string, int>, "
        "  od: OwnedDictionary<string, V>, "
        "  arr: Array<int>, "
        "  s: Set<int>, "
        "  n: int"
        ") ") + body + " end M; end C;";
}

} // namespace

// ===========================================================================
// §4.8 v0.135 — Dictionary<K,V> direct entry iteration is NOW ACCEPTED (#548)
// ===========================================================================

// §4.8 v0.135 (#548): `for var e in d` on a Dictionary<string,int> parameter
// must now be ACCEPTED — no error, loop variable typed as DictionaryEntry<K,V>.
// (Previously rejected per v0.134 #530; widened in v0.135 to allow entry iter.)
BEZEL_TEST(sema_dict_iteration_530, dict_direct_iteration_now_accepted_v0135_spec_4_8) {
    auto r = parseSemaAll(wrap("for var e in d do n := n + 1; end;"));
    // v0.135: Dictionary<K,V> entry iteration is valid — no compile error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // And the old confusing message is still absent.
    BEZEL_EXPECT_TRUE(noDiagContains(r, "not yet supported in codegen"));
}

// §4.8 v0.135 (#548): `for var e in d do ... e.Value ...` must be accepted.
// entry.Value is typed as V (int for Dictionary<string,int>), so the loop body
// can read it without compile error.
BEZEL_TEST(sema_dict_iteration_530, dict_entry_value_access_accepted_v0135_spec_4_8) {
    auto r = parseSemaAll(wrap("for var e in d do n := n + e.Value; end;"));
    // v0.135: DictionaryEntry.Value is V = int; adding to n (int) is valid.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.8 v0.135 (#548): `for var e in d do ... e.Key ...` must be accepted.
// entry.Key is typed as K (string for Dictionary<string,int>).
BEZEL_TEST(sema_dict_iteration_530, dict_entry_key_access_accepted_v0135_spec_4_8) {
    // e.Key is string; use it in a harmless way (no value-use of the key).
    auto r = parseSemaAll(wrap("for var e in d do var k: string := e.Key; n := n + 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §4.9 — OwnedDictionary<K,V> direct iteration must also be rejected
// ===========================================================================

// §4.9 v0.135: `for var e in od` on an OwnedDictionary<string,int> parameter
// must still be REJECTED (the #530 diagnostic is narrowed to OwnedDictionary
// only in v0.135; Dictionary is now accepted).
BEZEL_TEST(sema_dict_iteration_530, owned_dict_direct_iteration_rejected_spec_4_9) {
    auto r = parseSemaAll(wrap("for var e in od do n := n + 1; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic must mention OwnedDictionary and §4.9.
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "OwnedDictionary"));
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

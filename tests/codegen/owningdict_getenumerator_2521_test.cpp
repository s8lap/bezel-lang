// Sema (via the AOT compile path) — issue #2521: a call to
// `.GetEnumerator()` on an `OwningDictionary<K, V>` (or a bare reference to
// `d.GetEnumerator`) ICE'd the compiler instead of producing a clean
// diagnostic. Per stdlib §4.9, `OwningDictionary<K, V>` declares:
//
//   public class OwningDictionary<K, V>: IDisposable where V: class
//
// It implements `IDisposable` ONLY — unlike the other five owning containers
// (`OwningArray`/`OwningStack`/`OwningQueue`/`OwningSet`/`OwningLinkedList`,
// all of which reach `IEnumerable<T>` via `IReadOnlyCollection<T>` per
// §4.5/§4.6/§4.10/§4.11), it does NOT implement `IEnumerable<T>` nor
// `IOwningEnumerable<T>`, so `GetEnumerator`/`GetAsyncEnumerator` are
// genuinely NOT members. The blanket enumerable-protocol exemption in
// typeCall's `allowedOwningMethod` (correct for the other five owning
// containers) admitted the call for OwningDictionary too, no block typed
// the result, and codegen fell back to the #2434 "lowerLocalDecl's
// initializer failed to lower" internal (degraded to a zero-initialized
// value) — the ICE this test pins.
//
// The fix rejects `GetEnumerator`/`GetAsyncEnumerator` on an
// `OwningDictionary` receiver in `resolveMemberAccess` (ExprTypecheck_Calls.cpp),
// mirroring the existing foreach-form §4.9 reject (ExprTypecheck.cpp). This
// test asserts on the diagnostic TEXT (per tests/README.md: runAot's
// WIFSIGNALED path synthesizes an Error diagnostic for a runtime crash too,
// so a genuine rejection must be distinguished by its message).

#include "support/aot_support.hpp"   // runAot — AOT compile path
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

bool hasInternalIce(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find("lowerLocalDecl's initializer") !=
                    std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── §4.9: the issue's exact repro — `d.GetEnumerator()` call form ───────

BEZEL_TEST(owningdict_getenumerator_2521,
           call_form_rejected_spec_4_9) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d[\"a\"] := new V(1); "
        "using var e := d.GetEnumerator(); "  // not a member per §4.9
        "return 0; "
        "end Main; end C;";
    // The §4.9 "not a member" diagnostic must fire...
    BEZEL_EXPECT_TRUE(
        hasError(program, "is not a member of `OwningDictionary"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""4.9"));
    // ...and the pre-fix #2434 ICE ("lowerLocalDecl's initializer failed to
    // lower") must NOT — that is the defect this fix replaces with a clean
    // reject.
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── §4.9: bare-reference form `var f := d.GetEnumerator;` ───────────────

BEZEL_TEST(owningdict_getenumerator_2521,
           bare_form_rejected_spec_4_9) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "d[\"a\"] := new V(1); "
        "var f := d.GetEnumerator; "  // bare reference — same defect shape
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "is not a member of `OwningDictionary"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── §4.9: GetAsyncEnumerator is likewise not a member (no IAsyncEnumerable) ──

BEZEL_TEST(owningdict_getenumerator_2521,
           async_form_rejected_spec_4_9) {
    const std::string program =
        "module M; "
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V; "
        "class C public static method Main(): int "
        "using var d := new OwningDictionary<string, V>(); "
        "using var e := d.GetAsyncEnumerator(); "
        "return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "is not a member of `OwningDictionary"));
    BEZEL_EXPECT_TRUE(!hasInternalIce(program));
}

// ── False-reject guard: GetEnumerator on the BORROWING Dictionary works ──
// #2471 made `d.GetEnumerator()` work on `Dictionary<K, V>` (it IS a member
// via ICollection<DictionaryEntry<K,V>> → IEnumerable<T>); this fix must not
// regress that. The borrowing Dictionary's GetEnumerator call must NOT
// produce the OwningDictionary §4.9 reject.
BEZEL_TEST(owningdict_getenumerator_2521,
           borrowing_dictionary_getenumerator_unaffected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 1; "
        "using var e := d.GetEnumerator(); "
        "var n := 0; "
        "while e.MoveNext() do n := n + e.Current.Value; end; "
        "return n; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        !hasError(program, "is not a member of `OwningDictionary"));
}

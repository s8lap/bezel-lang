// Codegen — fix #3761 / stdlib §5, §4.8 (v0.286):
// `Dictionary<K, V>.Single()` typed correctly by sema but
// `LlvmBackend_Calls.cpp`'s `isDict` block had NO `Single` arm (only
// First/Last/ElementAt from narrow #3576), so the call fell through to
// `nullptr` and `lowerLocalDecl`'s #2434 defense-in-depth zero-filled the
// local SILENTLY. Issue #3761 measured `dictSingle=0` on a 1-entry dict —
// the program compiled, linked, ran, and printed 0, with only an internal
// "degraded to a zero-initialized value" note (not an error, so the
// silent wrong value was never caught at compile time).
//
// THE FIX. Add a `m == "Single"` arm to the same predicate the #3576
// First/Last/ElementAt arm sits inside, with the spec's throwing contract
// (stdlib §5: "`First`/`Last`/`Single`/`ElementAt` throw
// `InvalidOperationException` (or `IndexOutOfRangeException` for
// `ElementAt`) on missing-element"). For `Single` "missing-element" means
// the source has anything other than exactly one entry — covered by a
// single `dtCnt0 != 1` guard that fires BEFORE the snapshot allocation,
// so the throw path leaks neither the keys- nor the values-spine (same
// "no allocation before reject" discipline the #3576 review-amendment
// enforced for First/Last). On the success path the arm reuses the SAME
// `bz_dict_keys_gen`/`bz_dict_values_gen` snapshot + `decodeDictGenericKeyAt`/
// `decodeDictValueAt` decode pair as First/Last/ElementAt (#3717 helpers,
// reused without change) and decodes the lone entry at index 0.
//
// NARROWED OFF AT THE TIME (mirroring #3576): a STRING key. The string-key
// leak was a pre-existing #3720 issue this fix did not touch when it
// landed — measured 200x under ASan/LSan, the entry's Value was correct
// but the rebuilt `bz_string` Key leaked on every call (the synthetic
// DictionaryEntry TypeRef has no field list for the per-local disposal
// pass to walk). At the time, `Dictionary<string, V>.Single()` rejected
// LOUDLY with the same diagnostic as First/Last/ElementAt rather than
// shipping the leak. #3720 has SINCE landed (a disposal hook joining the
// `collectBorrowingArrayFreeEligible` family) and lifted that reject —
// see the `dictionary_string_key_single_now_works_3720` row below, which
// replaced this file's own loud-reject pin once it stopped reproducing.
//
// SCOPE. Plain `Dictionary<K, V>` is the affected receiver (#3761 only
// mentions the borrowing form). `SortedDictionary<K, V>.Single()` reaches
// the same codegen arm via the shared `isDictTypeName` registry and works
// identically (the §4.17 ascending-key sort the arm already applies for
// SortedDictionary is exactly what `Single()` should honor there too,
// matching `for var e in sd do`). `OwningDictionary<K, V>` is rejected
// loudly by sema (#3716, stdlib §4.9's "interface conformance is
// IDisposable only" prose rules out entry-view iteration on it).
//
// ORACLE. Plain-return rows assert the ENTRY yielded (Key*100+Value, so
// a wrong key or a wrong value both move the number). Throwing-contract
// rows are runAotChecked (200x loop, ASan/LSan) so a per-throw leak is
// visible; empty/multi both throw InvalidOperationException, which is
// the SAME class as First/Last (#3576 row "dictionary_first_empty_throws_5")
// so the leak guard matters for the same reason — the snapshot's
// `bz_dict_keys_gen`/`bz_dict_values_gen` mallocs must come AFTER the
// guard passes.
//
// REVIEW NOTE. FAIL-WITHOUT discipline: revert the source hunk (drop
// `m == "Single"` from the predicate) on this lane's rebase, rebuild, and
// the row 1 output is `dictSingle=0` — the silent miscompile pinned.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool hasError(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// T1 — issue #3761 repro: 1-entry Dictionary<int,int>.Single() must yield
// Key=3, Value=30 (encoded 330). Pre-fix: dictSingle=0 (#2434 silent
// degradation). Post-fix: dictSingle=330.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_single_one_entry_5_4_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(3, 30); "
        "var fe := d.Single(); "
        "Console.WriteLine($\"dictSingle={fe.Key * 100 + fe.Value}\"); "
        "return 0; ")),
        "dictSingle=330");
}

// ---------------------------------------------------------------------------
// T2 — control: First() on the same 1-entry dict (the existing #3576 path).
// No-op pin — verifies the Single fix does not disturb First's lowering.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_first_control_4_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(3, 30); "
        "var fe := d.First(); "
        "Console.WriteLine($\"dictFirst={fe.Key * 100 + fe.Value}\"); "
        "return 0; ")),
        "dictFirst=330");
}

// ---------------------------------------------------------------------------
// T3 — control: Last() on the same 1-entry dict (the existing #3576 path).
// No-op pin — verifies the Single fix does not disturb Last's lowering.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_last_control_4_8) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := new Dictionary<int, int>(); "
        "d.Add(3, 30); "
        "var le := d.Last(); "
        "Console.WriteLine($\"dictLast={le.Key * 100 + le.Value}\"); "
        "return 0; ")),
        "dictLast=330");
}

// ---------------------------------------------------------------------------
// T4 — empty dict Single() must throw InvalidOperationException. The throw
// fires BEFORE this fix's snapshot allocation (same "no allocation before
// reject" discipline as #3576's empty-First row), so runAotChecked proves
// the throw path leaks neither spine — measured pre-fix-as-applied would
// show 3200 B / 400 allocations per 200 throws, identical to the #3576
// review-amendment's pre-fix measurement.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_single_empty_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    try var fe := d.Single(); return fe.Key; "
        "    catch InvalidOperationException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

// ---------------------------------------------------------------------------
// T5 — multi-entry dict Single() must throw InvalidOperationException.
// `Single` is stricter than `First`/`Last`: even with entries present, the
// count must be exactly 1 (stdlib §5 + §5.1's "throws unless
// exactly one matches" wording for Single(pred)). 200x under ASan/LSan
// proves the throw path leaks no snapshot spine arrays either.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_single_multi_throws_5) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 200 do "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); d.Add(2, 20); "
        "    try var fe := d.Single(); return fe.Key; "
        "    catch InvalidOperationException => caught := caught + 1; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine($\"caught={caught}\"); "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "caught=200"));
}

// ---------------------------------------------------------------------------
// Bonus — OwningDictionary<K,V>.Single() must be rejected loudly (sema
// #3716: stdlib §4.9 rules out entry-view iteration on an owning dict —
// "a borrowing look-don't-take entry view over an owning dictionary is
// a separate decision deliberately not taken here"). The diagnostic must
// be raised BEFORE any allocation (same #3720-shaped pre-allocation
// reject the string-key case uses).
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761,
           owning_dictionary_single_rejected_loudly_3716) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class R public Tag: int := 0; end R; "
        "class C public static method Main(): int "
        "  var od := new OwningDictionary<int, R>(); "
        "  od.Add(1, new R(7)); "
        "  var fe := od.Single(); "
        "  return fe.Key; "
        "end Main; end C;",
        "`OwningDictionary<K, V>` does not expose `Single`"));
}

// ---------------------------------------------------------------------------
// Bonus — String-keyed Dictionary<K,V>.Single() now WORKS. This row used to
// pin a loud reject (stdlib §5 / §4.8, "... is not yet supported (#3720)");
// fix #3720 (LlvmBackend_Calls.cpp / LlvmBackend_Disposal.cpp / LlvmBackend_
// Setup.cpp, v0.294) gave the returned DictionaryEntry's rebuilt `bz_string`
// Key a disposal hook (dictEntryKeyReleaseEligible_, joining the
// collectBorrowingArrayFreeEligible family) and deleted the `if (!keyTy)`
// reject this row exercised, so asserting `hasError(...)` on the removed
// diagnostic text can never pass again. The value oracle below is the
// direct replacement: for a 1-entry dict keyed "a", `fe.Key.Length` is 1.
// Leak-freedom for this exact shape is already pinned 200x under ASan/LSan
// by the sibling row `dict_first_last_elementat_3576.
// dictionary_string_key_single_no_leak_3720`, landed in the same commit —
// this row only needs to prove the VALUE is right, not re-prove the leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(dictionary_single_3761, dictionary_string_key_single_now_works_3720) {
    BEZEL_EXPECT_EQ(runAot(prog(
        "var d := new Dictionary<string, int>(); "
        "d.Add(\"a\", 1); "
        "var fe := d.Single(); "
        "return fe.Key.Length; ")).exitCode,
        1);
}

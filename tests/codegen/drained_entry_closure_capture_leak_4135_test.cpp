// Codegen — fix #4135 (§27.5 Closures / §28.9 Runtime-managed types / stdlib
// §4.9 DrainedEntry<K,V>, v0.295): an escaping-closure capture of a
// `DrainedEntry<K,V>` local (from `for var entry in d.Drain()`) leaks its
// owned Value and, for string keys, its owned Key — the DrainedEntry sibling
// of #4127 (DictionaryEntry<string,V>).
//
// ROOT CAUSE: the eligibility walk that registers scope-end owned-field
// releases for a DrainedEntry local (analogous to
// isFreshDictEntryKeyReleaseTerminalLocal for DictionaryEntry) treats ANY
// closure capture as an escape and therefore never registers the disposal.
// Since nothing else discharges the owned fields for an escaping capture,
// both Key (if string) and Value leak. The `freeCellInEnvdtor`-gated release
// that #4127 added for DictionaryEntry<string,V> needs a DrainedEntry sibling:
// a DrainedEntry local's capture cell holds the `{K key; V* value}` struct BY
// VALUE, so loading field 0 (Key) and field 1 (Value) off the cell base and
// releasing them is structurally identical to the DictionaryEntry case.
// freeCellInEnvdtor's preconditions (singleCaptureLocals_ ∧
// definitelyEscapingLambdas_) guarantee no double-release from a second holder.
//
// SCOPE. This fix covers the for-loop-variable DrainedEntry capture ONLY.
// A plain local `var e := d.Drain().First()` binding shape (analogous to the
// plain DictionaryEntry `var fe := d.First()` in #4127) is a separate, NOT
// YET TESTED gap — if it exists, it would be the DrainedEntry analogue of
// #4127 itself. Only the for-loop-variable shape, documented in the issue's
// own repro, is covered here.
//
// THE FIX. Add isDrainedEntryTypeRef predicate (LlvmBackend_impl.hpp) mirroring
// isDictEntryStringKeyTypeRef, then add a DrainedEntry sibling branch in
// lowerLambda's envdtor (LlvmBackend_Exprs2.cpp) that releases: (a) the Key
// (bz_string_release for string keys, per isStringTypeRefNonNull) and (b) the
// Value (emitDisposeAndReclaim on the V* at field 1 — V is always a class per
// stdlib §4.9, so it lowers to ptr and needs emitDisposeAndReclaim).
//
// ORACLE. runAotChecked (ASan+LSan) loop-amplified 500x per LANE_RULES so a
// one-shot pass cannot masquerade as a fix. The pre-fix leak is 51 B / 3
// allocations per call (32 B capture cell + 16 B Widget + 3 B key payload);
// PASS-WITH is clean.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The issue's own verbatim repro: a for-loop-variable DrainedEntry<string,Widget>
// entry moved to `e` and captured in a directly-returned lambda. Value N=9,
// Key.Length=2, so f() = 11. 500x amplified.
std::string makeDrainedEntryClosureProgram() {
    return
        "module M; class Widget "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    using var d := new OwningDictionary<string, Widget>(); "
        "    d.Add(\"xy\", new Widget(9)); "
        "    for var entry in d.Drain() do "
        "      var e := move entry; "
        "      return lambda() => e.Key.Length + e.Value.N; "
        "    end; "
        "    return lambda() => 0; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do "
        "      var f := C.MakeClosure(); "
        "      total := total + f(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Value correctness (plain runAot — unaffected by this fix either way).
// "xy" -> Key.Length=2, Value.N=9, f()=11, total=500*11=5500.
// ---------------------------------------------------------------------------

BEZEL_TEST(drained_entry_closure_capture_leak_4135,
           string_key_value_closure_escaping_value_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget; "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    using var d := new OwningDictionary<string, Widget>(); "
        "    d.Add(\"xy\", new Widget(9)); "
        "    for var entry in d.Drain() do "
        "      var e := move entry; "
        "      return lambda() => e.Key.Length + e.Value.N; "
        "    end; "
        "    return lambda() => 0; "
        "  end MakeClosure; "
        "  public static method Main(): int return C.MakeClosure()(); end Main; "
        "end C;"),
        11);
}

// ---------------------------------------------------------------------------
// Leak coverage (runAotChecked, 500x amplified) — FAIL-WITHOUT / PASS-WITH.
// ---------------------------------------------------------------------------

// #4135 core repro, 500x. FAIL-WITHout: 51 B / 3 allocations per call
// (32 B capture cell + 16 B Widget + 3 B key payload "xy").
// PASS-WITH: clean.
BEZEL_TEST(drained_entry_closure_capture_leak_4135,
           string_key_value_captured_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeDrainedEntryClosureProgram());
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}

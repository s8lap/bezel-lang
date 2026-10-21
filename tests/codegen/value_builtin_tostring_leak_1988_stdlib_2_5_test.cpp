// Codegen / leak — the Named value-builtin `ToString()` family
// (DateTime / TimeSpan / DateOnly / TimeOnly) leaked its result string, the
// SAME +1-producer gap as #1982 (Guid), family-wide. Issue #1988.
//
// Spec: stdlib §2.5 (`DateTime.ToString(): string` ISO 8601 "yyyy-MM-ddTHH:mm:ss"
// and `ToString(format)`; `TimeSpan.ToString()`; `DateOnly.ToString()` "yyyy-MM-dd";
// `TimeOnly.ToString()` "HH:mm:ss"), stdlib §2.6 (`Guid.ToString()` — the #1982
// regression guard), language spec §4.6 / §28.9 (runtime-managed `string` refcount
// discipline: a producer hands the caller exactly +1; the consumer transfers-or-
// releases it once). Spec revision v0.197.
//
// THE BUG. A consumer of a string value (return / assign / bind) decides once, for
// the whole value, whether it is a +1 PRODUCER (transfer the reference) or a +0
// BORROW (mint one via bz_string_retain). The no-arg `ToString()` on each of these
// Named value types lowers (LlvmBackend_Calls.cpp per-type dispatch) to a runtime
// that mints a fresh heap `bz.string` at rc := 1 — a genuine +1 producer:
//   DateTime.ToString() / .ToString(format) → bz_datetime_to_string_fmt (rc=1)
//   TimeSpan.ToString()                      → bz_timespan_to_string_c (rc=1)
//   DateOnly.ToString()                      → bz_datetime_to_string_fmt (rc=1)
//   TimeOnly.ToString()                      → bz_timeonly_to_string (rc=1)
// But `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) recognized only the numeric
// PRIMITIVE ToString(), StringBuilder.ToString(), and (post-#1982) Guid — NOT this
// family: DateTime/TimeSpan/DateOnly/TimeOnly are Named value types. So
// `var s := d.ToString()` was treated as a borrow and RETAINED at bind (rc 1->2),
// then released at scope end (rc 2->1) — the producer's original +1 was never
// discharged and the whole result leaked (LSan: 32-byte bz_string header + the
// formatted buffer; DateTime = 112 bytes).
//
// THE FIX. isPlusOneStringProducer recognizes a no-arg `ToString()` on a
// DateTime/TimeSpan/DateOnly/TimeOnly receiver (and DateTime's `ToString(format)`,
// which routes through the SAME rc=1 runtime) as a +1 producer — the SAME shape
// lowerCall's per-type dispatch keys on — so the consumer TRANSFERS the single
// reference instead of minting a second. No double-free: no extra retain is
// emitted, and the one reference the runtime mints the scope-end release drops
// exactly once.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan; a
// leaked string flips r.ran to false (a double-free / UAF from over-releasing would
// too). Each case renders in a LOOP and counts matches: the loop makes all but the
// final allocation unreachable, so LSan RELIABLY reports the leak pre-fix (a single
// bound-then-read string can linger in a register and evade LSan's reachability
// scan — a false negative that would make a one-shot test flaky). The returned
// count == the iteration count IFF every render is byte-correct AND the run is
// leak-clean, so the exit code pins the VALUE and r.ran pins leak-freedom. Each
// case FAILS without the fix (LSan reports the leaked strings -> r.ran false) and
// PASSES with it. The Guid case is the #1982 regression guard (must stay clean).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Build a "render N times, count byte-correct results" program for a value-builtin.
// `decl` binds the receiver; `render` is the ToString call; `expected` is the exact
// canonical rendering. Returns 40 iff every render matched AND the run is leak-clean.
std::string loopCount(const char* decl, const char* render, const char* expected) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           decl +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var s := " + render + "; "
           "      if s = \"" + expected + "\" then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// (A) #1988 repro — DateTime.ToString() (no-arg, ISO 8601). stdlib §2.5 / §28.9.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           datetime_tostring_noarg_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(loopCount(
        "var v := DateTime.FromUnixTimeSeconds(0); ",
        "v.ToString()", "1970-01-01T00:00:00"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);           // compiled + LSan-clean (result freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 40);    // every render byte-correct
}

// =====================================================================
// (B) DateTime.ToString(format) overload — SAME bz_datetime_to_string_fmt (rc=1).
// stdlib §2.5.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           datetime_tostring_format_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(loopCount(
        "var v := DateTime.FromUnixTimeSeconds(0); ",
        "v.ToString(\"yyyy-MM-dd\")", "1970-01-01"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (C) TimeSpan.ToString() (no-arg). stdlib §2.5 / §28.9.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           timespan_tostring_noarg_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(loopCount(
        "var v := TimeSpan.FromSeconds(90.0); ",
        "v.ToString()", "00:01:30"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (D) DateOnly.ToString() (no-arg, "yyyy-MM-dd"). stdlib §2.5 / §28.9.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           dateonly_tostring_noarg_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(loopCount(
        "var v := new DateOnly(2024, 3, 15); ",
        "v.ToString()", "2024-03-15"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (E) TimeOnly.ToString() (no-arg, "HH:mm:ss"). stdlib §2.5 / §28.9.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           timeonly_tostring_noarg_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(loopCount(
        "var v := new TimeOnly(13, 45, 30); ",
        "v.ToString()", "13:45:30"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// =====================================================================
// (F) Guid.ToString() (#1982 regression guard — must stay leak-clean). stdlib §2.6.
// =====================================================================
BEZEL_TEST(value_builtin_tostring_leak_1988,
           guid_tostring_noarg_still_clean_stdlib_2_6) {
    JitResult r = runAotChecked(loopCount(
        "var v := Guid.Empty; ",
        "v.ToString()", "00000000-0000-0000-0000-000000000000"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

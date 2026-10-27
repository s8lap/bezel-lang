// Codegen / leak — a fresh +1 Guid-string producer whose ONLY use is a BORROW
// argument to Guid.Parse / Guid.TryParse leaked: the temp's +1 reference was
// never dropped. Issue #1998.
//
// Spec: language spec §28.3 "Unbound owned temporaries" (v0.226) — the section
// enumerates "a non-`owned` argument" as a borrowing use an unbound owned
// temporary may serve before being "disposed at the end of the full
// expression"; §4.6 / §28.9 (runtime-managed `string` refcount discipline: a
// producer hands the caller exactly +1, the consumer transfers-or-releases it
// once); stdlib §2.6 (`Guid.Parse(s: string): Guid` and
// `Guid.TryParse(s: string, out result: Guid): bool` take `s` by borrow;
// `Guid.ToString(): string` and `Guid.ToString(format: string): string` return
// a fresh `string` — the §2.18 "D"/"N" vocabulary). Spec revision v0.226.
//
// THE BUG. `Guid.Parse` / `Guid.TryParse` lower (LlvmBackend_Calls.cpp Guid
// block) to bz_guid_parse / bz_guid_try_parse, which only READ their `str`
// argument (guid_parse_str reads s->data to decode 16 bytes) — the parameter
// carries no `owned` marker (stdlib §2.6), so the call transfers NO ownership.
// A fresh +1 string producer passed as that argument — the issue's repro
// `Guid.Parse(g.ToString())`, where g.ToString() mints an rc=1 heap bz.string
// (recognized as a producer by #1982; the #1989 format overload g.ToString("N")
// likewise) — is the producer-temp-consumed-only-by-a-borrow-parameter shape.
// §28.3's unbound-temporary rule requires such a temp to be "disposed at the
// end of the full expression"; Parse returns a Guid and TryParse a bool, so
// nothing chains off the argument past the call — the call's own end IS that
// boundary. But the Guid block lowered the argument and immediately dropped it
// on the floor: nothing released the +1, and the value is not bound to a name
// (so no scope-end release reaches it either). The 32-byte bz_string header +
// the 37-byte canonical buffer leaked, every call (the #1998 repro: 69
// bytes/iteration under ASan/LSan; the format "N" form leaks the 32-byte header
// + a 33-byte buffer = 65 bytes/iteration). A LITERAL argument (e.g.
// Guid.Parse("...")) was already clean — a string literal is an immortal rc=0
// sentinel that bz_string_release skips — which is why the #1982 round-trip
// test (which bound the ToString result to a local) did not exercise this path.
//
// THE FIX. After the consuming bz_guid_parse / bz_guid_try_parse call, if the
// argument was a +1 string producer (isPlusOneStringProducer — the SAME
// recognizer that classifies the bind-site transfer-vs-retain decision), emit a
// single emitStringRelease (bz_string_release) on the temp. The string has been
// fully read by guid_parse_str before the runtime returns (ok is 1/0), so the
// reference is dead from that point on. A non-producer argument (a string
// local/field, or an immortal rc=0 literal) is NOT released here — its real
// owner reclaims it, and releasing a borrow would be a double-free. This is the
// runtime-managed-string counterpart of the #1881 borrowing-collection-Add and
// #2204 Insert/indexer-set arg-temp disposal (which use
// isFreshOwnedClassProducer + emitDisposeAndReclaim for class temps). No
// double-free: the producer mints exactly one reference and the release drops
// it exactly once.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string flips r.ran to false. Each case renders in a LOOP and counts
// byte-correct round-trips: the loop makes all but the final allocation
// unreachable, so LSan RELIABLY reports the leak pre-fix (a single Parse call's
// string can linger in a register and evade LSan's reachability scan — a false
// negative that would make a one-shot test flaky). The returned count == the
// iteration count IFF every round-trip is byte-correct AND the run is
// leak-clean, so the exit code pins the VALUE and r.ran pins leak-freedom.
// Each case FAILS without the fix (LSan reports the leaked strings → r.ran
// false) and PASSES with it. The literal-argument control (D) was already clean
// and pins that the fix introduces no double-free of an immortal literal.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// The exact #1998 repro: `Guid.Parse(g.ToString())` — the no-arg ToString
// producer (rc=1 heap string, #1982) passed as a BORROW to Parse, in a loop so
// LSan reliably reports the per-iteration leak. Counts byte-correct round
// trips: the parsed Guid equals the source Guid IFF the canonical "D" rendering
// round-trips. Returns 40 IFF every render round-tripped AND the run is
// leak-clean.
const char* kParseNoArgToString =
    "module M; class C "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 40 do "
    "      var g: Guid := Guid.NewGuid(); "
    "      var g2: Guid := Guid.Parse(g.ToString()); "
    "      if g = g2 then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "
    "  end Main; "
    "end C;";

// The #1989 format overload: `Guid.Parse(g.ToString("N"))` — ToString("N")
// mints a 32-char no-hyphen rc=1 string (recognized as a producer by #1989),
// passed as a BORROW to Parse, which accepts the "N" rendering (§2.18, v0.197).
// Same leak shape as the no-arg form. Round-trips IFF the "N" render parses
// back to the source Guid.
const char* kParseFormatToString =
    "module M; class C "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 40 do "
    "      var g: Guid := Guid.NewGuid(); "
    "      var g2: Guid := Guid.Parse(g.ToString(\"N\")); "
    "      if g = g2 then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "
    "  end Main; "
    "end C;";

// The TryParse sibling: `Guid.TryParse(g.ToString(), out r)` — same borrow-arg
// shape (bz_guid_try_parse only reads s->data), same leak pre-fix. The bool
// result returns by value, so the call's end is the full-expression boundary
// for the argument (§28.3). Counts round-trips where TryParse succeeded AND the
// parsed Guid equals the source.
const char* kTryParseNoArgToString =
    "module M; class C "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 40 do "
    "      var g: Guid := Guid.NewGuid(); "
    "      var r: Guid := Guid.Empty; "
    "      if Guid.TryParse(g.ToString(), out r) and r = g then "
    "        count := count + 1; "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "
    "  end Main; "
    "end C;";

// Control: Parse with a LITERAL argument — a string literal is an immortal
// rc=0 sentinel that bz_string_release skips, so this was already clean. The
// companion guard pins that the fix introduces no double-free of a non-producer
// (immortal) argument, and that the round-trip value is still correct.
const char* kParseLiteralControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var count := 0; "
    "    var i := 0; "
    "    while i < 40 do "
    "      var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "      var g2: Guid := Guid.Parse(g.ToString()); "
    "      if g = g2 then count := count + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    return count; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1998 repro — `Guid.Parse(g.ToString())` is LSan-clean. stdlib §2.6 /
// §28.3 / §28.9. Fails without the fix (69 bytes/iteration leaked).
// =====================================================================
BEZEL_TEST(guid_parse_borrow_temp_leak_1998,
           parse_noarg_tostring_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseNoArgToString);
    BEZEL_EXPECT_TRUE(r.ran);         // compiled + LSan-clean (temp freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // every "D" render round-tripped
}

// =====================================================================
// (B) Format overload — `Guid.Parse(g.ToString("N"))` is LSan-clean.
// stdlib §2.6 / §2.18 / §28.3. Fails without the fix (65 bytes/iteration).
// =====================================================================
BEZEL_TEST(guid_parse_borrow_temp_leak_1998,
           parse_format_tostring_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseFormatToString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // every "N" render round-tripped
}

// =====================================================================
// (C) TryParse sibling — `Guid.TryParse(g.ToString(), out r)` is LSan-clean.
// stdlib §2.6 / §28.3. Fails without the fix (same borrow-arg leak).
// =====================================================================
BEZEL_TEST(guid_parse_borrow_temp_leak_1998,
           tryparse_noarg_tostring_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kTryParseNoArgToString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // every TryParse succeeded + round-tripped
}

// =====================================================================
// (D) Control — Parse with a literal argument was already clean; pins that the
// fix introduces no double-free of an immortal (rc=0) argument. stdlib §2.6.
// =====================================================================
BEZEL_TEST(guid_parse_borrow_temp_leak_1998,
           parse_literal_control_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseLiteralControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);  // round-trips, no double-free of the literal
}

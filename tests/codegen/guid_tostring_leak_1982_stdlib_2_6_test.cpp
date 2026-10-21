// Codegen / leak — `Guid.ToString()` (and the `Guid.Parse` → `ToString`
// round-trip) leaked its result string. Issue #1982.
//
// Spec: stdlib §2.6 (`Guid.ToString(): string` — the canonical 36-char dashed
// form; `Guid.Parse(s): Guid`), language spec §4.6 / §28.9 (runtime-managed
// `string` refcount discipline: a producer hands the caller exactly +1, the
// consumer transfers-or-releases it once). Spec revision v0.197.
//
// THE BUG. A consumer of a string value (return / assign / bind) decides once,
// for the whole value, whether it is a +1 PRODUCER (transfer the reference) or a
// +0 BORROW (mint one via bz_string_retain). The no-arg `guid.ToString()` lowers
// (LlvmBackend_Calls.cpp Guid block) to bz_guid_to_string, which mints a fresh
// heap `bz.string` at rc := 1 (BZ_GUID_STRING_RC_INIT) — a genuine +1 producer.
// But `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) recognized the numeric
// `ToString()` (a PRIMITIVE receiver) and StringBuilder.ToString(), NOT the Guid
// one: Guid is a Named value type. So `var s := g.ToString()` was treated as a
// borrow and RETAINED at bind (rc 1->2), then released at scope end (rc 2->1) —
// the producer's original +1 was never discharged and the whole result leaked:
// LSan reports the 32-byte bz_string header + the 37-byte canonical-form buffer
// (69 bytes). `int.ToString()` bound the same way is clean because its numeric
// receiver IS recognized; the gap was specific to the Guid (Named) receiver.
//
// THE FIX. isPlusOneStringProducer recognizes a no-arg `ToString()` on a Guid
// receiver (CallExpr, empty args, member "ToString", Guid-typed target — the SAME
// shape lowerCall's Guid dispatch keys on) as a +1 producer, so the consumer
// TRANSFERS the single reference instead of minting a second. No double-free: no
// extra retain is emitted, and the runtime mints exactly one reference the
// scope-end release drops exactly once.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan; a
// leaked string flips r.ran to false (and a double-free / UAF from over-releasing
// would too). The exit code pins the correct value (via `.Length` = 36 for the
// canonical form, and a round-trip string compare), so a premature free would
// corrupt the value as well as flip r.ran. Each ToString case FAILS without the
// fix (LSan reports the leaked 69 bytes) and PASSES with it. The Parse-only case
// is a companion guard: Parse itself was already leak-clean (the literal argument
// is immortal), and this pins that the round-trip introduces no double-free of
// the argument string.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) The exact #1982 repro: `var s := g.ToString()` on a plain non-nullable
// Guid, consumed non-escaping. Pre-fix the minted canonical string leaked (69
// bytes). `.Length` (= 36) pins the value AND consumes the string.
const char* kBindToStringLength =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Empty; "
    "    var s := g.ToString(); "
    "    return s.Length; "          // 36 = canonical "xxxxxxxx-...-xxxxxxxxxxxx"
    "  end Main; "
    "end C;";

// (B) ToString VALUE correctness on the Empty Guid, bound then compared. Encodes
// leak-freedom (r.ran) AND the exact canonical rendering. Pre-fix: leaks → ran=false.
const char* kBindToStringValue =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Empty; "
    "    var s := g.ToString(); "
    "    if s = \"00000000-0000-0000-0000-000000000000\" then return 42; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) The Parse → ToString ROUND-TRIP: parse a known canonical Guid, render it
// back, and verify the string round-trips to the SAME lowercase canonical form.
// Exercises Parse and ToString together; pins leak-freedom of the ToString result
// AND no double-free of the parsed value / argument string. Pre-fix: the ToString
// result leaks → ran=false.
const char* kParseToStringRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    var s := g.ToString(); "
    "    if s = \"6ba7b810-9dad-11d1-80b4-00c04fd430c8\" then return 99; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) Parse ALONE (no ToString) — a companion guard that Parse is leak-clean and
// does not double-free the (immortal literal) argument string. Parse writes the
// 16 bytes in place and allocates nothing, so this was already clean; it locks
// that the round-trip fix leaves it clean.
const char* kParseOnly =
    "module M; class C "
    "  public static method Main(): int "
    "    var g: Guid := Guid.Parse(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\"); "
    "    var e: Guid := Guid.Empty; "
    "    if g <> e then return 7; end; "   // parsed != Empty
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #1982 repro — `var s := g.ToString()` is LSan-clean. stdlib §2.6 / §28.9.
// =====================================================================
BEZEL_TEST(guid_tostring_leak_1982,
           bind_tostring_length_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kBindToStringLength);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (result freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 36); // canonical dashed form length
}

// =====================================================================
// (B) ToString value correctness + leak-freedom. stdlib §2.6.
// =====================================================================
BEZEL_TEST(guid_tostring_leak_1982,
           bind_tostring_value_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kBindToStringValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // Empty renders the all-zero canonical form
}

// =====================================================================
// (C) Parse → ToString round-trip — leak-clean, no double-free, value preserved.
// stdlib §2.6 / §28.9.
// =====================================================================
BEZEL_TEST(guid_tostring_leak_1982,
           parse_tostring_round_trip_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseToStringRoundTrip);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);  // round-trips to the same canonical string
}

// =====================================================================
// (D) Parse alone — companion guard: leak-clean, no double-free of the argument.
// stdlib §2.6.
// =====================================================================
BEZEL_TEST(guid_tostring_leak_1982,
           parse_only_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kParseOnly);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

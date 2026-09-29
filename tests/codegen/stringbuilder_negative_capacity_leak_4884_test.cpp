// Regression test for #4884 (stdlib §1/§3.1, v0.307): `new StringBuilder(n)`
// with n < 0 correctly raises ArgumentOutOfRangeException (already pinned by
// tests/codegen/negative_capacity_aoor_1396_test.cpp), but leaked its backing
// bz_array_t — a 56-byte header plus a 512-byte 64-slot data buffer (568 B / 2
// allocations per throwing call) — because the `StringBuilder` constructor
// lowering in LlvmBackend_Exprs2.cpp called `bz_array_alloc` BEFORE the
// negative-capacity `emitGuardThrow`. When the guard fired, the already-
// allocated array was abandoned: the constructor never returns a value for a
// caller to dispose.
//
// Contrast with the correctly-ordered sibling sized constructors in the same
// file: `OwningBuffer<T>(n)` and `Array<T>(size, fill)` both run
// `emitGuardThrow` on the negative-magnitude check BEFORE calling
// `bz_array_alloc_count_n`. `StringBuilder(capacity)` was the one sized
// constructor stdlib §1's "Negative magnitudes fault" bullet names that got
// the order backwards.
//
// THE FIX: move the negative-capacity guard ahead of the `bz_array_alloc`
// call, mirroring the sibling ctors' ordering. The exception's identity and
// catchability are unchanged (still pinned by the #1396 test file above);
// this test adds the LEAK-FREEDOM assertion that file never made.
//
// CLAUDE.md's "ship the loop" guidance: a single-shot LSan repro can pass
// vacuously if the one abandoned allocation happens to stay reachable from a
// live stack slot or register. This test loops the throwing construction 100
// times so a real per-call leak scales linearly and cannot hide behind a
// one-shot reachability artifact (measured pre-fix: 56800 B / 200 allocations
// for 100 iterations — see FAIL-WITHOUT below).
//
// Spec: stdlib §1 "Negative magnitudes fault" (v0.307): "capacity/size/length
// < 0 raises ArgumentOutOfRangeException on the StringBuilder, Array<T>,
// OwningArray<T>, Dictionary/OwningDictionary/Set/OwningSet capacity forms and
// OwningBuffer<T>. Zero is valid."

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Plain AOT run (value/behavior only, no sanitizer). Wraps a bare statement
// body in the module/class/Main() boilerplate, matching the helper in
// tests/codegen/negative_capacity_aoor_1396_test.cpp.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Generated-code ASan + LeakSanitizer (runAotChecked): a leaking program
// reports ran=false (the LSan report fires and the sentinel -999 is
// returned); a leak-clean program returns its real exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

std::string runCheckedStdout(const std::string& program, bool* ranOut) {
    auto r = runAotChecked(program);
    if (ranOut) *ranOut = r.ran;
    return r.stdoutText;
}

}  // namespace

// #4884 primary repro: `new StringBuilder(-5)` thrown 100 times in a loop,
// each iteration caught by `ArgumentOutOfRangeException`. Before the fix this
// leaked 568 B / 2 allocations PER iteration (56800 B / 200 allocations total,
// measured) and runAotChecked's LSan leg failed with ran=false. After the
// fix the loop is leak-clean and the program's own exit code (the count of
// caught exceptions, 100) is observed.
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, loop100_leak_clean) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var caught: int := 0; "
        "    for var i in 0..<100 do "
        "      try "
        "        var sb := new StringBuilder(-5); "
        "      catch "
        "        var e: ArgumentOutOfRangeException => caught := caught + 1; "
        "      end try; "
        "    end for; "
        "    Console.WriteLine($\"caught={caught}\"); "
        "    return caught; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(runChecked(program), 100);
}

// Same program's exact stdout, independent of the exit-code channel (which
// runAotChecked's ASAN_OPTIONS pins to the program's own value per #4638, but
// this is a second, redundant observable of the same fact — every one of the
// 100 throwing constructions was caught, none fell through silently).
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, loop100_stdout_exact) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var caught: int := 0; "
        "    for var i in 0..<100 do "
        "      try "
        "        var sb := new StringBuilder(-5); "
        "      catch "
        "        var e: ArgumentOutOfRangeException => caught := caught + 1; "
        "      end try; "
        "    end for; "
        "    Console.WriteLine($\"caught={caught}\"); "
        "    return caught; "
        "  end Main; "
        "end C; ";
    bool ran = false;
    const std::string out = runCheckedStdout(program, &ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, std::string("caught=100\n"));
}

// Control (stdlib §1, "Zero is valid"): `new StringBuilder(0)` must still
// construct successfully and be leak-clean — the fix reordered the guard
// relative to the allocation but must not disturb the non-throwing path.
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, zero_capacity_still_valid_and_clean) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(0); "
        "    sb.Append(\"x\"); "
        "    return sb.Length; "
        "  end Main; "
        "end C; "), 1);
}

// Control (fix #531, stdlib §3.1): the string-initial-value constructor path
// (`new StringBuilder(initial: string)`) is untouched by the reordering —
// the isStrArg branch is now evaluated and executed AFTER the allocation
// exactly as before, just gated by a bool computed earlier. Still leak-clean.
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, string_initial_ctor_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(\"hi\"); "
        "    return sb.Length; "
        "  end Main; "
        "end C; "), 2);
}

// Control (no-arg constructor): `new StringBuilder()` is unaffected (neither
// branch of the reordered code fires; straight to alloc).
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, noarg_ctor_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    sb.Append(\"ab\"); "
        "    return sb.Length; "
        "  end Main; "
        "end C; "), 2);
}

// Sanity (plain runAot, no sanitizer): the single-shot exception-identity
// contract already pinned by negative_capacity_aoor_1396_test.cpp still
// holds after the reordering — this fix must not change WHAT is thrown, only
// WHEN the allocation happens relative to the guard.
BEZEL_TEST(stringbuilder_negative_capacity_leak_4884, single_shot_still_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var sb := new StringBuilder(-5); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Spec-derived regression test for issue #3154.
//
// §5.1 (Implicit numeric conversions, v0.254) makes `int` -> `long` an implicit
// widening. stdlib §2.7 (v0.252) declares `public constructor(seed: long);` on
// `Random`. So `new Random(<int-typed expression>)` is a well-typed program and
// must compile — the seed value must be sign-extended to i64 before being
// handed to the `bz_random_alloc(i64)` runtime call.
//
// THE DEFECT (measured on `main` @ `701cb2d`, reproduced on
// `e766cb6`). LlvmBackend_Exprs2.cpp's `new Random` arm lowered the seed
// argument and discarded the type check's outcome — it assigned to a local
// `int64_t seed = 0` that was never read, then passed the narrower value
// unchanged. `bezelc -c` reported:
//
//   Call parameter type does not match function signature!
//   i32 7
//    i64  %1 = call ptr @bz_random_alloc(i32 7)
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// `new Random(7L)` (long literal) and `new Random()` (no arg) compiled and ran
// — only the int-expression path broke. Other built-in constructors with `long`
// parameters (`new Atomic<long>(1)`, ordinary user methods with `(long)`
// parameters) widen correctly, so this is the `Random` constructor arm
// specifically, not a §5.1 regression.
//
// Each test below asserts `r.ran == true` (program ran) and a specific
// `r.exitCode`. Pre-fix: `r.ran == false` because LLVM's module verifier
// rejected the i32-against-i64 call, so bezelc aborts at compile time.
// Post-fix: the call is well-typed, the program links and runs.
//
// Per the lane protocol: the test name carries the bug id (`3154`) and the
// shape (`random_int_seed_malformed_ir`) so a later triage grep finds both the
// failing test and the offending lowering arm in the same search.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// §5.1 v0.254 / stdlib §2.7 v0.252 / #3154: int LITERAL seed must widen.
// `new Random(7)` — int literal 7 — must compile to an `i64` argument at the
// `bz_random_alloc` call site. Pre-fix: bezelc emits `i32` and LLVM's verifier
// rejects the call.
BEZEL_TEST(codegen_random_int_seed_widening_3154,
           random_int_seed_malformed_ir_int_literal) {
    JitResult r = runAot(prog(
        "var rng := new Random(7); "
        "var v := rng.NextInt64(); "
        // We don't depend on the PRNG's specific output — we just need the
        // program to RUN (ran==true). Pre-fix the compile aborts with
        // "IR verification failed" and `r.ran` stays false.
        "if v = v then return 11; end; "
        "return 0;"
    ));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// §5.1 v0.254 / stdlib §2.7 v0.252 / #3154: int LOCAL seed must widen.
// `new Random(n)` where `n: int` — the int expression came from a local, not a
// literal. The buggy arm lowered both equally (passed the narrower IR value
// through); the fix sign-extends whenever the value isn't already i64.
BEZEL_TEST(codegen_random_int_seed_widening_3154,
           random_int_seed_malformed_ir_int_local) {
    JitResult r = runAot(prog(
        "var n: int := 12345; "
        "var rng := new Random(n); "
        "var v := rng.NextInt64(); "
        "if v = v then return 13; end; "
        "return 0;"
    ));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// §5.1 v0.254 / stdlib §2.7 v0.252 / #3154: int EXPRESSION seed must widen.
// `new Random(m + 1)` where the seed is a binary expression — the lowered IR
// value is the sum, still i32. The fix must sign-extend the result of `lowerExpr`
// uniformly regardless of how the i32 was derived.
BEZEL_TEST(codegen_random_int_seed_widening_3154,
           random_int_seed_malformed_ir_int_expr) {
    JitResult r = runAot(prog(
        "var m: int := 99; "
        "var rng := new Random(m + 1); "
        "var v := rng.NextInt64(); "
        "if v = v then return 17; end; "
        "return 0;"
    ));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 17);
}

// CONTROL — `new Random(42L)` (long literal) already worked pre-fix; the fix
// must not break the already-correct arm. §2.7's constructor parameter is
// `seed: long`; passing an already-long literal must NOT sign-extend a value
// that is already i64.
BEZEL_TEST(codegen_random_int_seed_widening_3154,
           random_int_seed_malformed_ir_long_literal_control) {
    JitResult r = runAot(prog(
        "var rng := new Random(42L); "
        "var v := rng.NextInt64(); "
        "if v = v then return 19; end; "
        "return 0;"
    ));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 19);
}
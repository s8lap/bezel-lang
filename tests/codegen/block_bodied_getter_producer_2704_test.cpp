// Codegen / refcounting — a STATEMENT-BODIED block getter whose body returns
// a +1 string PRODUCER (an interpolation, a `+`, etc.) leaked the producer's
// reference at every read. Issue #2704.
//
// Spec (revision v0.263):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." The `do ... end` accessor is a user statement body
//     whose `return` terminates the ACCESSOR. The accessor forms are
//     semantically indistinguishable at every read site ("Accessor faces
//     are uniform across type kinds", the §28.6 runtime-managed carve-out).
//   §28.6 / §28.9 "Runtime-managed types" — `string` is runtime-managed
//     (reference counted): every co-owner takes exactly one reference and
//     releases exactly one. An unbalanced producer at scope end is a leak.
//   §28.9 — "A getter body whose result is a fresh runtime-managed
//     production (an interpolation, a concatenation, a library call)
//     remains admissible — runtime-managed values are exempt from the
//     ceremony — and the implementation reclaims the production; how is
//     not specified."
//
// THE BUG. The expression-bodied twin (`get => $"p-{_s}"`) is handled by
// #2304's producer recogniser (`isPlusOneStringProducer`, in
// `LlvmBackend_Setup.cpp`): the read site (the computedGetterOf path in
// `LlvmBackend_Exprs.cpp`) inlines the body in the CALLER's scope, so the
// producer's single +1 reference flows naturally to the caller's local
// and `armStringLocalRefcount` releases it at scope end. The statement-
// bodied twin (`get => do return $"p-{_s}"; end`) was NOT — `computedGetterOf
// returns null for a statement body, so the read site took the
// `blockGetterOf` path (~LlvmBackend_Exprs.cpp:5433), which CALLS the
// getter as a separate function and hands the call's result back as a
// BORROW. The producer's +1 reference reaches the caller's local via the
// return value, but the call site treats it as a borrow: no scope-end
// release, leak at every read.
//
// THE FIX (#2704, v0.263). The producer recogniser in
// `LlvmBackend_Setup.cpp` (`isPlusOneStringProducer`) is extended to also
// recognise a statement-bodied block getter whose body is exactly one
// statement, `return <producer>;`, where the producer is itself a +1
// producer. The getter itself stays a +0-contract read face (#2694's
// decision holds in lockstep — `currentReturnTypeNode_` is still null in
// the getter body, so emitReturn does not mint); the producer's single
// +1 reference flows straight through the return to the caller. The
// caller's extended recognition is what transfers the +1 to the caller's
// local and routes its scope-end release via `armStringLocalRefcount`.
// A borrow-bodied getter (`return _s;`) is unchanged: still classified as
// false, the caller still treats the read as a borrow (retain at bind,
// release at scope end — exactly the pre-fix behaviour).
//
// ⚠ NARROW FIX — DELIBERATELY. Only bodies that are EXACTLY ONE STATEMENT
// and that statement is `return <producer>;` are now recognised. A body
// with intermediate locals or branches can still produce a borrow at any
// single point we don't statically know, so it is conservatively left for
// the borrow path (no leak there, no double-release either).
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds
// to an RC-SENTINEL string and `bz_string_release` is a NO-OP on a
// sentinel, so a literal-built probe passes whether or not the bug is
// present. Every program below builds its producer with interpolation at
// run time (`$"v-{i}"`).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// =====================================================================
// The issue's repro, byte-identical. A block-bodied (`get => do … end`)
// `string` getter whose body returns an interpolation `$"p-{_s}"` — the
// +1 producer the read site previously never discharged. 20 reads, 20
// leaks (40 allocations, two per producer: the bz.string header +
// its data buffer).
//
// Pre-fix (HEAD ~e73424d):  SUMMARY: AddressSanitizer: 771 byte(s) leaked
//                          in 40 allocation(s) ; exit 1 (LSan failure).
// Post-fix:                 exit 111, no LSan report.
// =====================================================================
const char* kBlockBodiedProducer =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return $\"p-{_s}\"; "
    "    end; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := h.S; "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 111
    "  end Main; "
    "end P;";

// =====================================================================
// (A) EXPRESSION-BODIED twin — `get => $"p-{_s}"`. Already passes today
// via #2304's `computedGetterOf` arm. Pinning it here keeps the fix from
// silently breaking the case the issue explicitly cites as the working
// twin. Value: 111.
// =====================================================================
const char* kExpressionBodiedProducer =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => $\"p-{_s}\"; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := h.S; "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 111
    "  end Main; "
    "end P;";

// =====================================================================
// (B) BLOCK-GETTER `+` PRODUCER — `return _s + "-x"`. The `+` concatenation
// is itself a +1 string producer (handled by an earlier arm of
// `isPlusOneStringProducer`), so this exercises the recogniser's recursion
// — the block-bodied getter arm added by #2704 must recurse on the
// producer and recognise `+` exactly the way it does for an
// expression-bodied getter. Value: 111 (length("v-i") + 2 = 5 or 6, plus
// the "-x" suffix makes it 7 or 8 — sum 8*7 + 12*8 = 56 + 96 = 152).
//
// Re-traced: i=1..9 → "v-1".."v-9" length 3 each = 9*3 = 27; suffix "-x"
// makes each length 5 → 9*5 = 45. i=10..20 → "v-10".."v-20" length 4 each
// = 11*4 = 44; suffix makes each length 6 → 11*6 = 66. Total = 45 + 66
// = 111.
// =====================================================================
const char* kBlockBodiedConcatProducer =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return _s + \"-x\"; "
    "    end; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := h.S; "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 111
    "  end Main; "
    "end P;";

// =====================================================================
// (C) BORROW-BODIED GETTER (CONTROL) — `return _s;`. A borrow return, NOT
// a producer. Must stay classified as false — the caller retains at bind
// (the borrow path) and releases at scope end; if the fix accidentally
// re-classified it as a producer the caller would fail to retain, the
// borrow would be released at scope end while `_s` is still owned by the
// `using var h` holder — a USE-AFTER-FREE on the next iteration's `t.Length`
// read. Value: 71 (sum of "v-1".."v-20" lengths, no suffix).
// =====================================================================
const char* kBlockBodiedBorrow =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return _s; "
    "    end; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := h.S; "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

// =====================================================================
// (D) BLOCK-GETTER WITH NON-SINGLE-STATEMENT BODY — control / negative.
// The body has more than one statement, so the producer recogniser
// deliberately stays in the borrow path (a body with intermediate
// locals or branches can still produce a borrow at any single point we
// don't statically know — conservatively left as a borrow). The borrow
// path retains at bind and releases at scope end, so the program
// balances correctly. This test pins that a fix that over-reached and
// tried to recognise multi-statement bodies would NOT help here —
// classifying `_s` (a field read in `x`) as a producer would surface as
// a UAF on `t.Length` once `_s` was released at `t`'s scope end while
// the holder still owned it. Value: 71.
// =====================================================================
const char* kBlockBodiedMultiStatement =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      var x := _s; "
    "      return x; "
    "    end; "
    "  end S; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := h.S; "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

}  // namespace

// =====================================================================
// THE BUG. The block-bodied producer return leaked its +1 reference at
// every read. Pre-fix this test fails with LSan reporting 771 B / 40
// alloc (exit 1). Post-fix the program runs to completion, returns 111,
// and reports no leak. §23.3 / §28.6 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_bodied_getter_producer_2704,
           block_bodied_string_producer_return_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(kBlockBodiedProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));   // ASan toolchain unavailable fails the row (#3733)
    BEZEL_EXPECT_TRUE(r.ran);          // no LSan leak
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

// =====================================================================
// (A) The expression-bodied twin — the working case the fix MUST NOT
// regress. §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_bodied_getter_producer_2704,
           expression_bodied_twin_still_clean_spec_23_3) {
    JitResult r = runAotChecked(kExpressionBodiedProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

// =====================================================================
// (B) The `+` producer variant — exercises the recogniser's recursion:
// the block-bodied arm must recurse into `isPlusOneStringProducer` and
// recognise `_s + "-x"` exactly as the expression-bodied arm does.
// §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_bodied_getter_producer_2704,
           block_bodied_concat_producer_return_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(kBlockBodiedConcatProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

// =====================================================================
// (C) CONTROL — borrow-bodied getter stays in the borrow path. A fix
// that over-reached and classified `_s` (a field read) as a producer
// would surface here as a UAF on `t.Length` once `_s` was released at
// `t`'s scope end while the holder still owned it. §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_bodied_getter_producer_2704,
           block_bodied_borrow_return_still_borrows_spec_23_3) {
    JitResult r = runAotChecked(kBlockBodiedBorrow);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (D) CONTROL — multi-statement body stays in the borrow path. The fix
// is deliberately narrow (single-statement bodies only); over-reaching
// here would either miss the producer (silently leak) or falsely
// classify a borrow as a producer (silently double-free the holder's
// `_s` on the next iteration). Value: 71 — the body just borrows and
// returns `_s` through `x`.
// =====================================================================
BEZEL_TEST(codegen_block_bodied_getter_producer_2704,
           block_bodied_multi_statement_body_stays_in_borrow_path_spec_23_3) {
    JitResult r = runAotChecked(kBlockBodiedMultiStatement);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

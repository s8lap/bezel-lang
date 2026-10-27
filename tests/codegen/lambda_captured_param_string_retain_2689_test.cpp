// Codegen / refcounting — a lambda capturing a borrowed `string` PARAMETER took
// no retain, while the generated `bz$lam$N$envdtor` released that capture at the
// closure's last drop. The very first such capture therefore over-released a
// string the CALLER still owns. Issue #2689.
//
// Spec: §28.1 "Where the `owned` modifier can appear" — "On any other type — a
// primitive, `string`, a function type, an ordinary record, an enum, a
// non-owning inline array, or the unit type — `owned` is a compile error: such
// values carry no disposal obligation to transfer (§28.9)" — so a `string` or
// function-typed parameter is ALWAYS an unmodified borrow; §28.9
// "Runtime-managed types" (`string` and function values are refcounted: every
// co-owner takes exactly one reference and releases exactly one); §27.5
// "Closures and captured variables" (captures are promoted to shared heap cells
// held by the closure env). Spec revision v0.237.
//
// THE BUG, AND WHY THE PARAMETER CASE DIFFERS FROM THE LOCAL CASE. The env
// destructor releases a capture cell's contents keyed only on the capture's sema
// type (LlvmBackend_Exprs2.cpp, the dtor emission). The compensating retain
// lived ONLY in the captured-LOCALS loop, and only on the 2nd-and-later capture
// (`alreadyBoxed`). That is correct FOR A LOCAL: escape detection suppresses the
// local's scope-end release, so the first env inherits that suppressed release
// and only additional envs must mint. A captured PARAMETER has nothing to
// inherit — §28.1 forbids `owned` on exactly these types, so the caller owns the
// reference and this frame has no scope-end release to hand over. The
// captured-PARAMS loop emitted no retain at all, so N captures produced N env
// releases against 0 retains:
//
//     AddressSanitizer: SEGV in __asan::Allocator::Deallocate
//       #2 bz_string_release  #3 bz$lam$0$envdtor  #4 bz_closure_release
//
// ⚠ THE ISSUE'S "ONE LAMBDA IS BALANCED BY ACCIDENT" CLAIM IS WRONG, and case
// (B) below is the mechanical correction: a SINGLE lambda over one borrowed
// `string` parameter already crashes. That is why the fix retains on EVERY
// parameter capture rather than only on `alreadyBoxed` ones as for locals.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL on which `bz_string_release` is a NO-OP, so a literal-built probe
// passes whether or not the bug is present. Every program below builds its
// strings with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THE SHARED STRING IS A PARAMETER AND NEVER A LOCAL ESCAPING INTO A
// COLLECTION. That shape hits the pre-existing #422 leak, which would flip
// `r.ran` on its own and make these cases vacuous in the leak direction.
//
// OBSERVABILITY. `runAotChecked` links and runs under ASan/LSan, so BOTH failure
// directions flip `r.ran`: the over-release (the bug) and a leak from an
// over-correction. The exit code independently pins string lengths read back
// after the captures, so a premature free corrupts the value regardless of the
// sanitizer.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) The #2689 repro verbatim: TWO lambdas capturing one borrowed `string`
// parameter. Pre-fix each env destructor released the caller's string, so the
// second free hit already-freed memory. 20 iterations so a stale free is hit
// repeatedly rather than tolerated once.
// Value: 2 * (sum of "v-1".."v-20" lengths = 71) = 142.
const char* kTwoLambdasCaptureStringParam =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    return f1(1).Length + f2(2).Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (B) ONE lambda over one borrowed `string` parameter — the correction to the
// issue's own analysis, and the case that proves the retain must fire on the
// FIRST capture and not only on `alreadyBoxed` ones. Pre-fix this SEGV'd too.
// Value: sum of "v-1".."v-20" lengths = 71.
const char* kOneLambdaCapturesStringParam =
    "module M; class C "
    "  public static method One(s: string): int "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    return f1(1).Length; "
    "  end One; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.One($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 71
    "  end Main; "
    "end C;";

// (C) OVER-CORRECTION GUARD — the captured-LOCAL path must stay untouched. The
// same two-lambda shape over an owned LOCAL string was already balanced by the
// transfer model (first capture inherits the suppressed scope-end release,
// second mints), and was verified green both before and after the fix. Had the
// retain been added to the locals loop as well, this would leak +1 per capture
// and LSan would fail it. Value: 142.
const char* kTwoLambdasCaptureStringLocal =
    "module M; class C "
    "  public static method Two(n: int): int "
    "    var s := $\"v-{n}\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    return f1(1).Length + f2(2).Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two(i); "
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (D) The FUNCTION-TYPED sibling of (A): §28.1 bars `owned` on a function type
// in the same sentence it bars it on `string`, the env destructor releases a
// function-typed capture in the same way, and the captured-params loop was
// missing that retain too. Pre-fix this reported
// `heap-use-after-free ... in bz$lam$0$envdtor`.
//
// ⚠ RUN ON THE PLAIN (non-LSan) PATH DELIBERATELY. A closure local passed as an
// argument is never released today — a PRE-EXISTING leak of the closure cell,
// unrelated to this fix and proven so by a control program that never captures
// `f` at all and leaks the identical 1040 bytes / 60 allocations. LSan would
// therefore fail this case for a defect it does not own. `runAot` still pins the
// use-after-free through the value: the captured closure is invoked AFTER the
// point at which the pre-fix env destructor had already released it.
// Value: sum over i in 1..10 of ((1+i) + (2+i)) = sum(3+2i) = 140. The loop is
// 10 and not 20 only to keep the total inside `exitCode`'s 8-bit WEXITSTATUS
// mask.
const char* kTwoLambdasCaptureClosureParam =
    "module M; class C "
    "  public static method Two(f: (int) -> int): int "
    "    var g1 := lambda(k: int) => do return f(k); end; "
    "    var g2 := lambda(k: int) => do return f(k); end; "
    "    return g1(1) + g2(2); "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      var h := lambda(x: int) => do return x + i; end; "
    "      total := total + C.Two(h); "
    "    end; "
    "    return total; "                            // 140
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2689 — two lambdas capturing one borrowed `string` parameter no longer
// over-release the caller's string. §28.1 / §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_param_string_retain_2689,
           two_lambdas_capturing_string_param_no_use_after_free_spec_28_1) {
    JitResult r = runAotChecked(kTwoLambdasCaptureStringParam);
    BEZEL_EXPECT_TRUE(r.ran);          // no over-release (bug) and no leak
    BEZEL_EXPECT_EQ(r.exitCode, 142);  // both captures still read back
}

// =====================================================================
// (B) A SINGLE lambda over a borrowed `string` parameter was ALREADY broken —
// the retain must fire on the first capture, not only on later ones. §28.1.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_param_string_retain_2689,
           one_lambda_capturing_string_param_no_use_after_free_spec_28_9) {
    JitResult r = runAotChecked(kOneLambdaCapturesStringParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (C) Over-correction guard: the captured-LOCAL transfer model is untouched and
// still leak-free. Green before and after by design. §27.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_param_string_retain_2689,
           two_lambdas_capturing_string_local_still_balanced_spec_27_5) {
    JitResult r = runAotChecked(kTwoLambdasCaptureStringLocal);
    BEZEL_EXPECT_TRUE(r.ran);         // no leak from an over-broad retain
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

// =====================================================================
// (D) The function-typed parameter sibling — same §28.1 sentence, same missing
// retain. Plain path on purpose: a pre-existing closure-cell leak (#422 family)
// owns the LSan signal here, so the value is the oracle. §28.1 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_param_string_retain_2689,
           two_lambdas_capturing_closure_param_no_use_after_free_spec_28_1) {
    JitResult r = runAot(kTwoLambdasCaptureClosureParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 140);
}

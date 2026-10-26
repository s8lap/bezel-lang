// Codegen / refcounting — a BLOCK-bodied lambda whose return type is a
// non-nullable `string` did not mint the +1 its EXPRESSION-bodied sibling
// mints, so the same lambda was safe or unsafe depending only on which body
// form the author wrote. Issue #2688.
//
// Spec: §27.3 "Lambda expressions" (expression body and block body are two
// spellings of one construct — the return-type ascription "behaves exactly as a
// method's declared return type", so the two forms carry the same return
// contract), §27.7 "Calling function values" (a call through a function value
// yields that contract's result), §28.9 "Runtime-managed types" (`string` is
// refcounted: every co-owner takes exactly one reference and releases exactly
// one), §28.11 "Collection element ownership" (a borrowing
// `Dictionary<K,string>` co-owns a +1 per stored value). Spec revision v0.237.
//
// THE BUG. `lowerLambda` (LlvmBackend_Exprs2.cpp) nulls `currentReturnTypeNode_`
// before emitting a lambda body — correct on its own terms (#2515: a lambda body
// must not box or mint against the ENCLOSING METHOD's return node), but it also
// switched off `emitReturn`'s `string` +1 mint (LlvmBackend_Stmts.cpp). The
// expression-body path was unaffected: it keys its mint off `retSemaTy`, the
// lambda's OWN return type (LlvmBackend_Exprs2.cpp). So:
//
//     lambda(k: string) => k                     →  returns +1   (balanced)
//     lambda(k: string) => do return k; end      →  returns +0   (BROKEN)
//
// Every caller that releases the result then double-freed the block-body form
// and only that form:
//   AddressSanitizer: SEGV in __asan::Allocator::Deallocate
//     #2 bz_string_release  #3 bz$strElemRelease  #4 bz_dict_free
// for the `Dictionary.GetOrAdd` store site named in #2688, and, with no
// dictionary involved at all, a plain `var t := f(s);` bind:
//   AddressSanitizer: SEGV ... #2 bz_string_release  #3 main
//
// THE FIX. Carry the lambda's own return type into `emitReturn` through
// `currentLambdaReturnSemaTy_` and mint against it when there is no declared
// return-type node, mirroring the expression-body predicate EXACTLY — same
// `!nullable` gate, same `isPlusOneStringProducer` /
// `returnedStringLocalOwnsPlusOne` transfer exclusions. The fix is in the
// CALLEE, not at `GetOrAdd`'s store: a factory may be an arbitrary closure value
// (`d.GetOrAdd(s, f)`), so a call site cannot in general see which body form
// produced it — only normalising the lambda's own result to +1 makes every
// caller correct.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL string and `bz_string_release` is a NO-OP on a sentinel, so a
// literal-built probe passes whether or not the bug is present. Every program
// below builds its string with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THE FACTORY RETURNS ITS OWN PARAMETER RATHER THAN A CAPTURE. Capturing a
// borrowed `string` parameter in a lambda is a SEPARATE, pre-existing
// use-after-free (the closure env destructor releases a reference the capture
// never took — filed separately), which would mask this one. Returning the
// lambda's own parameter keeps these programs capture-free, so the only
// refcount question they pose is the return mint.
//
// OBSERVABILITY. `runAotChecked` links and runs under ASan/LSan, so BOTH failure
// directions flip `r.ran` to false: a double-free/SEGV (too few mints — the bug)
// and a leak (too many — an over-correction). The exit code independently pins
// values read back out of the dictionaries, so a premature free corrupts it
// regardless of the sanitizer.

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

// (A) The #2688 repro. Two borrowing `Dictionary<string,string>` locals each
// take the same runtime-built string through `GetOrAdd` with a BLOCK-bodied
// factory that returns its own key parameter (a borrow). Each dictionary's
// scope-end disposal releases a +1 per stored value (§28.11); pre-fix the
// factory minted none, so the second release freed a string the first had
// already freed. ONE dictionary alone is balanced by accident, which is why the
// defect went unnoticed — hence two. 20 iterations so a stale free is hit
// repeatedly rather than tolerated once.
// Value: 2 * (sum of "v-1".."v-20" lengths = 71) = 142.
const char* kGetOrAddBlockBodyFactory =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var d1 := new Dictionary<string, string>(); "
    "    var d2 := new Dictionary<string, string>(); "
    "    _ := d1.GetOrAdd(s, lambda(k: string) => do return k; end); "
    "    _ := d2.GetOrAdd(s, lambda(k: string) => do return k; end); "
    "    return d1[s].Length + d2[s].Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (B) The EXPRESSION-body spelling of the very same program. It was already
// balanced (its mint pairs with the dictionaries' releases) and must stay so —
// this is the over-correction guard for the `GetOrAdd` side: a fix that instead
// retained at the store site would double-retain here and LSan would fail it.
// §27.3 requires the two spellings to agree, and (A) and (B) now do: both 142.
const char* kGetOrAddExpressionBodyFactory =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var d1 := new Dictionary<string, string>(); "
    "    var d2 := new Dictionary<string, string>(); "
    "    _ := d1.GetOrAdd(s, lambda(k: string) => k); "
    "    _ := d2.GetOrAdd(s, lambda(k: string) => k); "
    "    return d1[s].Length + d2[s].Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (C) NO DICTIONARY AT ALL — the defect is in the lambda, not in `GetOrAdd`.
// A block-bodied lambda returning a borrow, called through a function value
// (§27.7) and bound to a local, double-freed pre-fix: the bind releases a +1 the
// callee never minted. This is what proves the fix belongs in the callee and
// pins the scope of the regression beyond the issue's own repro.
// Value: sum of "v-1".."v-20" lengths = 71.
const char* kClosureCallBlockBodyBorrowReturn =
    "module M; class C "
    "  public static method Use(s: string): int "
    "    var f := lambda(k: string) => do return k; end; "
    "    var t := f(s); "
    "    return t.Length; "
    "  end Use; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Use($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 71
    "  end Main; "
    "end C;";

// (D) The TRANSFER arm: the block body returns a fresh +1 PRODUCER
// (interpolation), which already owns its single reference, so the fix must NOT
// retain — it must transfer, exactly as the expression-body path does via
// `isPlusOneStringProducer`. An unconditional mint would leak one string per
// iteration and LSan would fail this.
// Value: 20 * 2 ("p-") + 71 = 111.
const char* kBlockBodyProducerReturnTransfers =
    "module M; class C "
    "  public static method Use(s: string): int "
    "    var f := lambda(k: string) => do return $\"p-{k}\"; end; "
    "    var t := f(s); "
    "    return t.Length; "
    "  end Use; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Use($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 111
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2688 — `Dictionary.GetOrAdd` with a BLOCK-bodied factory returning a
// borrow no longer double-frees the stored string. §27.3 / §28.9 / §28.11.
// =====================================================================
BEZEL_TEST(codegen_lambda_block_body_string_return_mint_2688,
           get_or_add_block_body_factory_no_double_free_spec_28_11) {
    JitResult r = runAotChecked(kGetOrAddBlockBodyFactory);
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (bug) and no leak
    BEZEL_EXPECT_EQ(r.exitCode, 142);  // both dictionaries still read back
}

// =====================================================================
// (B) The expression-body spelling of (A) is unchanged and still balanced —
// §27.3's two body forms now agree, both 142. Over-correction guard.
// =====================================================================
BEZEL_TEST(codegen_lambda_block_body_string_return_mint_2688,
           get_or_add_expression_body_factory_still_balanced_spec_27_3) {
    JitResult r = runAotChecked(kGetOrAddExpressionBodyFactory);
    BEZEL_EXPECT_TRUE(r.ran);         // no leak from a double-retain
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

// =====================================================================
// (C) The same defect with NO dictionary: a block-bodied lambda's borrow return
// bound through a function-value call. §27.3 / §27.7 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_block_body_string_return_mint_2688,
           closure_call_block_body_borrow_return_no_double_free_spec_27_7) {
    JitResult r = runAotChecked(kClosureCallBlockBodyBorrowReturn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (D) A +1 producer body is TRANSFERRED, not retained — pins the
// isPlusOneStringProducer half of the fix (an unconditional mint leaks here).
// §27.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_block_body_string_return_mint_2688,
           block_body_producer_return_transfers_without_extra_mint_spec_28_9) {
    JitResult r = runAotChecked(kBlockBodyProducerReturnTransfers);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 111);
}

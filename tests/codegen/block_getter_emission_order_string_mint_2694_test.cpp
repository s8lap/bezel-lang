// Codegen / refcounting — a lazily-emitted, MEMOIZED block-bodied property or
// indexer getter inherited the CALLER's return contract, so a `string`-typed
// getter spuriously minted a +1 inside its own body. Issue #2694.
//
// Spec (revision v0.240):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." The `do ... end` accessor is a user statement body whose
//     `return` terminates the ACCESSOR, so the accessor carries its own return
//     contract; it does not inherit the contract of whatever code reads it.
//   §23.4 "Indexers" — the same accessor forms for the indexer spelling.
//   §27.3 "Lambda expressions" — a lambda's return-type ascription is a return
//     contract of its own (the #2688 half of this defect: a block-bodied
//     `string`-returning lambda now carries `currentLambdaReturnSemaTy_`).
//   §28.9 "Runtime-managed types" — `string` is runtime-managed (reference
//     counted): every co-owner takes exactly one reference and releases exactly
//     one. An unbalanced mint is a leak.
//
// THE BUG. `ensurePropertyBlockGetter` (LlvmBackend_Assign.cpp) and
// `ensureIndexerBlockGetter` (same file) emit the getter as a standalone
// function, LAZILY, from the middle of expression lowering, and MEMOIZE it
// (`propBlockGetterFns_`). They save ~20 emitter fields around that emission —
// but saved NEITHER `currentReturnTypeNode_` NOR (since #2688)
// `currentLambdaReturnSemaTy_`. So the getter's own `return _s;` reached
// emitReturn's `string` +1 mint (LlvmBackend_Stmts.cpp) carrying the *reader's*
// return contract, and minted a retain INSIDE the getter.
//
// Neither call site discharges that +1: the property-read path
// (LlvmBackend_Exprs.cpp) and the indexer-read path (LlvmBackend_Assign.cpp)
// both hand the call's result straight back as a BORROW, and #2304's producer
// recognizer (LlvmBackend_Setup.cpp) deliberately does NOT classify a
// statement-bodied block getter as a +1 producer. So the getter's contract is
// +0 and the inherited mint leaks. Leak direction only, never a UAF.
//
// ⚠ WHY THIS IS AN EMISSION-ORDER TEST, NOT A LEAK TEST. The getter is emitted
// once and cached, so only the FIRST reader to be lowered decides its body — and
// the loop that lowers method bodies iterates
// `std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_`
// (LlvmBackend_impl.hpp), i.e. POINTER-HASH order, not declaration order.
// The getter's ABI was therefore a function of hash-map iteration order.
// Confirmed mechanically on the pre-fix compiler with BEZEL_DUMP_IR: the two
// programs below (A) and (B) are identical except for the ORDER of the two
// reader methods, and the emitted `bz$prop$S$get` differed —
//     LenOf-first:  load ptr; call void @bz_string_retain(ptr %2); ret ptr %2
//     StrOf-first:  load ptr;                                      ret ptr %2
// — with only the retaining order leaking (LSan: 731 bytes in 40 allocations =
// 20 strings, one per loop iteration). Post-fix the two are byte-identical.
// Asserting BOTH orders are clean is what pins the invariant: a test that
// exercised one call site could not tell a fixed getter from a getter that
// merely happened to hash the harmless way. It is also why the test does not
// depend on WHICH order is the leaking one — if libstdc++'s bucket order ever
// flips, one of (A)/(B) still fails without the fix.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL string and `bz_string_release` is a NO-OP on a sentinel, so a
// literal-built probe passes whether or not the bug is present. Every program
// below builds its string with interpolation at run time (`$"v-{i}"`).
//
// THE FIX. Save, null, and restore `currentReturnTypeNode_` and
// `currentLambdaReturnSemaTy_` across the body emission in BOTH getters —
// the same save/null/restore the #2170 fix already applies to `smKind_` /
// `smStateStruct_` in the property getter, for the same stated reason: a getter
// must not inherit the caller's emission mode.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 6 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// The shared subject: a class with a `string` field and a BLOCK-bodied
// (`get => do … end`) `string` property returning that field — a BORROW, so the
// getter must mint nothing. `using var` gives the holder a definite scope end so
// the field's reference is released deterministically; the ctor/field/dispose
// round trip on its own is balanced (verified in isolation).
const char* kHolder =
    "module M; "
    "class Holder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return _s; "
    "    end; "
    "  end S; "
    "end Holder; ";

// Two readers of the SAME property: one in an `int` return contract (which mints
// nothing) and one in a `string` return contract (which mints +1 for a borrow).
// Whichever is lowered first freezes the cached getter's body.
const char* kLenOf =
    "  public static method LenOf(h: Holder): int "
    "    return h.S.Length; "
    "  end LenOf; ";
const char* kStrOf =
    "  public static method StrOf(h: Holder): string "
    "    return h.S; "
    "  end StrOf; ";

// Calls both readers, 20 iterations, on a runtime-built string.
// Value: 2 * (sum of "v-1".."v-20" lengths = 71) = 142.
const char* kMainBoth =
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "   // runtime-built, NOT folded
    "      total := total + P.LenOf(h); "
    "      var t := P.StrOf(h); "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 142
    "  end Main; ";

std::string orderIntFirst() {
    return std::string(kHolder) + "class P " + kLenOf + kStrOf + kMainBoth +
           "end P;";
}
std::string orderStrFirst() {
    return std::string(kHolder) + "class P " + kStrOf + kLenOf + kMainBoth +
           "end P;";
}

// The pre-#2688 half in isolation: the getter's ONLY reader is a `string`-
// returning METHOD, so `currentReturnTypeNode_` alone decides the getter's body
// — deterministic, no reliance on hash order. Pre-fix this leaked.
// Value: sum of "v-1".."v-20" lengths = 71.
const char* kOnlyStringMethodReader =
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
    "  public static method StrOf(h: Holder): string "
    "    return h.S; "
    "  end StrOf; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      var t := P.StrOf(h); "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

// The #2688 half: the getter's ONLY reader is a BLOCK-bodied `string`-returning
// LAMBDA. `lowerLambda` nulls `currentReturnTypeNode_` (#2515), so pre-#2688
// this shape did not trigger; #2688 added `currentLambdaReturnSemaTy_`, which
// the getter then inherited. This is the context that fix widened the hole by.
// Value: 71.
const char* kOnlyLambdaReader =
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
    "      var f := lambda(x: Holder): string => do return x.S; end; "
    "      var t := f(h); "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

// The INDEXER sibling (§23.4). `ensureIndexerBlockGetter` had the identical
// hole; its read path likewise returns the call result as a borrow. Value: 71.
const char* kIndexerStringMethodReader =
    "module M; "
    "class Bag "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public property [i: int]: string "
    "    get => do "
    "      return _s; "
    "    end; "
    "  end; "
    "end Bag; "
    "class P "
    "  public static method StrAt(b: Bag): string "
    "    return b[0]; "
    "  end StrAt; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var b := new Bag($\"v-{i}\"); "
    "      var t := P.StrAt(b); "
    "      total := total + t.Length; "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

// CONTROL / over-correction guard: the getter's only reader is an `int` return
// contract, which never minted. This shape was already correct and must stay
// correct — a fix that removed a mint the getter genuinely owed, or that left
// the getter under-retained, would surface here as an ASan use-after-free rather
// than a leak. Value: 71.
const char* kOnlyIntReader =
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
    "  public static method LenOf(h: Holder): int "
    "    return h.S.Length; "
    "  end LenOf; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      using var h := new Holder($\"v-{i}\"); "
    "      total := total + P.LenOf(h); "
    "    end; "
    "    return total; "                              // 71
    "  end Main; "
    "end P;";

}  // namespace

// =====================================================================
// (A) / (B) THE EMISSION-ORDER INVARIANT. The same program with its two reader
// methods declared in swapped order must behave identically — the memoized
// getter's body may not depend on which reader is lowered first.
// §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_emission_order_2694,
           int_reader_declared_first_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(orderIntFirst());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // no leak from a getter-internal mint
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

BEZEL_TEST(codegen_block_getter_emission_order_2694,
           string_reader_declared_first_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(orderStrFirst());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

// =====================================================================
// (C) The pre-#2688 half, deterministic: the getter's only reader is a
// `string`-returning METHOD (`currentReturnTypeNode_`). §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_emission_order_2694,
           getter_read_only_from_string_method_does_not_mint_spec_28_9) {
    JitResult r = runAotChecked(kOnlyStringMethodReader);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (D) The #2688 half: the getter's only reader is a BLOCK-bodied
// `string`-returning LAMBDA (`currentLambdaReturnSemaTy_`). §27.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_emission_order_2694,
           getter_read_only_from_string_lambda_does_not_mint_spec_27_3) {
    JitResult r = runAotChecked(kOnlyLambdaReader);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (E) The INDEXER sibling — `ensureIndexerBlockGetter` had the same hole.
// §23.4 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_emission_order_2694,
           indexer_block_getter_read_from_string_method_does_not_mint_spec_23_4) {
    JitResult r = runAotChecked(kIndexerStringMethodReader);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (F) CONTROL: the already-correct `int`-contract reader stays correct — the
// getter is still a +0 borrow, not under-retained into a UAF. §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_emission_order_2694,
           int_contract_reader_control_stays_balanced_spec_23_3) {
    JitResult r = runAotChecked(kOnlyIntReader);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

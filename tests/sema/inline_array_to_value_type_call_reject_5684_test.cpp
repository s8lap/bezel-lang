// Spec-derived regression test for issue #5684 — §5.1 / §8.6, v0.313.
//
// ROOT CAUSE: `checkCallArity` for non-generic single-overload calls returned
// without calling `checkArgsAgainstParams` after `bindArgsToParams` succeeded,
// so argument types were never checked against parameter types. An InlineArray
// argument at a primitive parameter (e.g. `d.Put(arr)` where `arr: [2]Item`
// and `Put(x: int)`) compiled clean through sema and reached codegen as
// malformed IR ("Call parameter type does not match function signature!" + LLVM
// verifier failure).
//
// The fix adds two changes:
// 1. ExprTypecheck_Calls.cpp (checkCallArity): call checkArgsAgainstParams
//    for non-generic single-overload calls.
// 2. ExprTypecheck.cpp (checkAssignable): add InlineArray→value-type rejection
//    at top level (was accidentally placed inside the Object-source block,
//    making it unreachable).
//
// CONTROLS — must all stay GREEN:
// (1) [N]T identity: var a: [2]int := [1, 2]
// (2) [N]T → [N]T same size: var b: [2]int := a (same size)
// (3) [N]T → [N2]T different size: var c: [3]int := a (different size,
//     existing rules handle this — spec §15.1 says distinct sizes don't convert)
// (4) InlineArray to a method that legitimately accepts it

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff compilation failed with an ERROR diagnostic carrying `needle`.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the program compiles, links, runs and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// ===========================================================================
// THE BUG — inline array at primitive parameter in non-generic single-overload
// call. Pre-fix: compiled with exit 0, no diagnostic, produced malformed IR.
// Post-fix: rejects with the specific diagnostic.
// ===========================================================================
const char* kInlineArrayAtPrimitiveRepro =
    "module M5684; "
    "public class Item5684 "
    "  public constructor() end constructor; "
    "end Item5684; "
    "public class D5684 "
    "  public constructor() end constructor; "
    "  public method Put(x: int) end Put; "
    "end D5684; "
    "public class C5684 "
    "  public static method Main(): int "
    "    var a := new Item5684(); "
    "    var b := new Item5684(); "
    "    var arr: [2]Item5684 := [a, b]; "
    "    var d := new D5684(); "
    "    d.Put(arr); "  // [2]Item5684 at int parameter
    "    return 0; "
    "  end Main; "
    "end C5684;";

// ===========================================================================
// CONTROL 1 — [N]T identity: inline array literal at matching declared type.
// Must still compile, link, run.
// ===========================================================================
const char* kIdentitySrc =
    "module M5684c1; "
    "public class C5684c1 "
    "  public static method Main(): int "
    "    var arr: [2]int := [1, 2]; "
    "    return arr[0] + arr[1]; "
    "  end Main; "
    "end C5684c1;";

// ===========================================================================
// CONTROL 2 — [N]T → [N]T same size: inline array variable at matching
// declared type. Must still compile, link, run.
// ===========================================================================
const char* kSameSizeSrc =
    "module M5684c2; "
    "public class C5684c2 "
    "  public static method Main(): int "
    "    var a: [2]int := [1, 2]; "
    "    var b: [2]int := a; "
    "    return b[0] + b[1]; "
    "  end Main; "
    "end C5684c2;";

// ===========================================================================
// CONTROL 3 — [N]T → [N2]T different size: this is handled by existing
// rules (spec §15.1: different sizes are distinct types, no conversion).
// Must REJECT (not compile) with a diagnostic.
// This is rule-corpus case s15_1_0005 — verified separately.
// ===========================================================================

// ===========================================================================
// CONTROL 4 — inline array to a method that legitimately accepts it.
// Must still compile, link, run.
// ===========================================================================
const char* kInlineArrayToMatchingParamSrc =
    "module M5684c4; "
    "public class Item5684c4 "
    "  public n: int; "
    "  public constructor() end constructor; "
    "end Item5684c4; "
    "public class D5684c4 "
    "  public constructor() end constructor; "
    "  public method Put(arr: [2]Item5684c4) end Put; "
    "end D5684c4; "
    "public class C5684c4 "
    "  public static method Main(): int "
    "    var a := new Item5684c4(); "
    "    var b := new Item5684c4(); "
    "    var arr: [2]Item5684c4 := [a, b]; "
    "    var d := new D5684c4(); "
    "    d.Put(arr); "  // [2]Item5684c4 at [2]Item5684c4 parameter — OK
    "    return 0; "
    "  end Main; "
    "end C5684c4;";

}  // namespace

// ===========================================================================
// THE BUG — inline array at primitive parameter in non-generic call.
// ===========================================================================
BEZEL_TEST(inline_array_to_value_type_call_reject_5684,
           inline_array_item_at_primitive_int_param_rejected) {
    // The specific diagnostic names the parameter type and cites §5.1, §8.6
    BEZEL_EXPECT_TRUE(rejectsWith(kInlineArrayAtPrimitiveRepro,
        "has type `[2]Item5684`, which has no implicit conversion to a primitive value type"));
    BEZEL_EXPECT_TRUE(rejectsWith(kInlineArrayAtPrimitiveRepro, "\xC2\xA7""5.1"));
    BEZEL_EXPECT_TRUE(rejectsWith(kInlineArrayAtPrimitiveRepro, "\xC2\xA7""8.6"));
}

// ===========================================================================
// CONTROL 1 — [N]T identity must still compile and run.
// ===========================================================================
BEZEL_TEST(inline_array_to_value_type_call_reject_5684,
           inline_array_literal_at_matching_type_accepted) {
    BEZEL_EXPECT_TRUE(runs(3, kIdentitySrc));
}

// ===========================================================================
// CONTROL 2 — [N]T → [N]T same size must still compile and run.
// ===========================================================================
BEZEL_TEST(inline_array_to_value_type_call_reject_5684,
           inline_array_same_size_assignment_accepted) {
    BEZEL_EXPECT_TRUE(runs(3, kSameSizeSrc));
}

// ===========================================================================
// CONTROL 4 — inline array to method that legitimately accepts it must still
// compile and run.
// ===========================================================================
BEZEL_TEST(inline_array_to_matching_param_type_accepted,
           inline_array_to_matching_param_accepted) {
    BEZEL_EXPECT_TRUE(runs(0, kInlineArrayToMatchingParamSrc));
}

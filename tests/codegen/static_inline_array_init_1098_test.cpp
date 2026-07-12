// Regression tests for #1098 (§15.1 / §7.5): a `static` field of inline-array
// type `[N]T` with an explicit initializer (`static _arr: [3]int := [5, 6, 7]`)
// was silently zero-initialized — the literal was dropped. The static-field-init
// codegen tried materializeHeapCollectionLiteral (null for a non-heap inline
// array) then lowerExpr (null for a CollectionLiteralExpr), then skipped the
// store. The fix stores the literal element-wise into the `[N x T]` global,
// mirroring the local inline-array init path. The same literal already worked
// for a local. Found by adversarial discovery sweep #7.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1098: the first element of a static inline-array initializer is applied.
BEZEL_TEST(static_inline_array_init_1098, first_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _arr: [3]int := [5, 6, 7]; "
        "  public static method Main(): int return C._arr[0]; end Main; "
        "end C;"), 5);
}

// All elements are initialized (not just slot 0).
BEZEL_TEST(static_inline_array_init_1098, all_elements) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _arr: [3]int := [5, 6, 7]; "
        "  public static method Main(): int "
        "    return C._arr[0] + C._arr[1] + C._arr[2]; "   // 5 + 6 + 7 = 18
        "  end Main; "
        "end C;"), 18);
}

// A computed index into the static array reads the initialized value.
BEZEL_TEST(static_inline_array_init_1098, computed_index) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _arr: [4]int := [10, 20, 30, 40]; "
        "  public static method Main(): int "
        "    var i := 2; "
        "    return C._arr[i]; "                           // 30
        "  end Main; "
        "end C;"), 30);
}

// Regression guard: a scalar static field initializer is unaffected.
BEZEL_TEST(static_inline_array_init_1098, scalar_static_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 42; "
        "  public static method Main(): int return C._n; end Main; "
        "end C;"), 42);
}

// Regression guard: a LOCAL inline-array initializer is unaffected.
BEZEL_TEST(static_inline_array_init_1098, local_inline_array_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr: [3]int := [5, 6, 7]; "
        "  return arr[0] + arr[2]; "                       // 5 + 7 = 12
        "end Main; end C;"), 12);
}

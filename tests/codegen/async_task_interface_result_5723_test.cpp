// Regression pins for issue #5723 (§30.5 "Result lifetime and task disposal",
// v0.338): an interface-typed `Task<T>` / `OwningTask<T>` result is classified
// with the class-result Pattern A / Pattern B rules.
//
// Two rows are pinned here, both matching the spec on current origin/main:
//
//  * reject: `using var r := await MakeAsync();` at `Task<IShape>` (Pattern A).
//    §30.5 Pattern A: the awaited borrow "may not flow to a `using var`
//    binding" (the same ban a class T gets; the class control
//    `Task<Sq>` produces the identical diagnostic on current main).
//
//  * accept + run: `OwningTask<IShape>` consumed by `using var` (Pattern B,
//    §30.5 "`OwningTask<T>` for class or interface T": the await transfers the
//    single owned result to the awaiter, which discharges it with `using var`).
//    Looped 50x under the generated-code sanitizer (runAotChecked) so a leak or
//    a double-free of the interface-typed result is visible.
//
// NOT pinned here: the Pattern A *unbound* accept `var r := await MakeAsync();`
// at `Task<IShape>` (probe in #5723). The class control `Task<Sq>` is rejected
// for that shape by §30.5's syntactically-visible-owning-location rule, but the
// interface shape is accepted and the awaited result is then used after free
// (heap-use-after-free under ASan in `Main$Resume`). That divergence is an
// ownership/async defect, reported on #5723 rather than encoded as a pin.

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kUsingVarBorrowDiagFragment =
    "the borrow produced by `await Task<T>` (Pattern A) may not flow to a "
    "`using var` binding";

constexpr int kLoopIters = 50;

constexpr const char* kIShape =
    "public interface IShape "
    "  method Area(): int; "
    "end IShape; "
    "public class Sq: IShape "
    "  public method Area(): int => 4; "
    "end Sq; ";

}  // namespace

BEZEL_TEST(async_task_interface_result_5723,
          reject_using_var_await_task_interface_pattern_a_30_5) {
    const std::string src =
        std::string("module M; ") + kIShape +
        "public class Program "
        "  public static async method MakeAsync(): Task<IShape> "
        "    return new Sq(); "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    using var r := await MakeAsync(); "
        "    return r.Area(); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kUsingVarBorrowDiagFragment));
}

BEZEL_TEST(async_task_interface_result_5723,
          accept_owningtask_interface_using_var_await_30_5) {
    const std::string src =
        std::string("module M; ") + kIShape +
        "public class Program "
        "  public static async method MakeAsync(): OwningTask<IShape> "
        "    return new Sq(); "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < " + std::to_string(kLoopIters) + " do "
        "      using var r := await MakeAsync(); "
        "      total := total + r.Area(); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 4 * kLoopIters); // 50 * Area() == 200 (<= 255)
}

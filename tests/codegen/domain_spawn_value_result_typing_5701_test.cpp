// Codegen — stdlib §6.8 / spec §30.14 (v0.338; issue #5701): `Domain.Spawn`
// over a non-class result R selects the `Spawn<R>(entry: () -> R): owned Task<R>`
// row, so the join is an ordinary `Task<R>` (Pattern A), not an `OwningTask<R>`.
// Spec §30.14 (docs/Bezel_Language_Spec.md, "Spawning") lists that row with the
// comment `// R: sendable non-class`; stdlib §6.8 declares the same signature.
//
// Covered here: a `Task<int>`-annotated binding over the join, awaited once,
// is admitted and yields the entry's value. The probed shape that awaits the
// same value-result join TWICE is NOT pinned here: on base e1ba6b44 it compiles
// but the second await raises InvalidOperationException at run time, while the
// same double await on `Task.FromResult(7)` returns 14. §30.14 says a `Task<R>`
// join follows Pattern A, so that is a divergence reported on #5701, not a
// shape this test may assert either way.

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;

namespace {

// (a) Task<int>-annotated join over a value result, awaited once.
const std::string kAnnotatedTaskJoin =
    "module M; "
    "public class C "
    "  public static method Count(): int => 7; "
    "  public static async method Main(): Task<int> "
    "    var t: Task<int> := Domain.Spawn(C.Count); "
    "    return await t; "
    "  end Main; "
    "end C; ";

}  // namespace

// Spec §30.14 (v0.338), Spawning: `Spawn<R>(entry: () -> R): owned Task<R>`
// with `R: sendable non-class`; the annotated `Task<int>` binding is admitted.
BEZEL_TEST(domain_spawn_value_result_typing_5701,
           task_annotated_join_value_result_spec_30_14) {
    JitResult r = runAotChecked(kAnnotatedTaskJoin);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}


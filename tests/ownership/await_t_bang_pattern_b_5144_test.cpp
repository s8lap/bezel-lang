// Spec-derived regression test for issue #5144 — Pattern B `await t!` over a
// plain owned local task. §17.8 (v0.309): "Over a named owned binding, `x!`
// is therefore classified by its position exactly as the bare read `x` is.
// At a marker-free position of the consume table — for example ... the
// operand of a Pattern B `await` — `await t!` consumes `t` exactly as
// `await t` does." §30.5 (Pattern B): `await <OwningTask<T>>` transfers
// ownership to the consumer; `await t` of a plain owned local task consumes
// the task local. `await t!` is the §17.8 transparent-`!` spelling of the
// same consume.
//
// Pre-fix `recordAwaitOperandConsume` matched only a bare IdentifierExpr
// operand, so `await t!` (PostfixBangExpr wrapping IdentifierExpr) skipped
// the consume — the second `await t!` of the same local would NOT trip the
// straight-line second-await diagnostic (it was a slipped read). After
// #5144 the consume runs identically for `await t` and `await t!`.
//
// Source shape mirrors `null_forgiving_owned_sink_1985.builtin_exc_local_
// bang_into_throw_*` (a marker-free consume) and the await test files in
// this area; ownership cell-counters are the dispose-once oracle.

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* kCell =
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// §17.8 / §30.5 (v0.309, #5144, #4993): `await t!` consumes t (Pattern B
// marker-free consume). Single await, no straight-line second await — the
// positive control that the bang form actually reaches the consume code
// path. Loop-amplified under ASan (runAotChecked) so a silently-missed
// consume that double-frees / leaks the Cell cannot pass by accident.
BEZEL_TEST(await_t_bang_pattern_b_5144,
           await_t_bang_consumes_spec_17_8) {
    std::string src =
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Mk(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Mk; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var t := C.Mk(); "
        "        var y := await t!; "    // §17.8 Pattern B consume
        "        y.Dispose(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.live; "         // 0: every Cell disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §17.8 / §30.4 (v0.309, #5144, #4993): the straight-line SECOND `await t!`
// is a read of a consumed task local (the §30.5 single-consumer spend) and
// must be rejected. This pins the consume: `t!` is NOT merely an alias for
// `t` — it actually transfers ownership like `t` does, so the second await
// is the §30.4 single-consumer violation. Pre-fix the consume missed (the
// postfix-`!` was not peeled), so the second await compiled and would have
// raised InvalidOperationException at runtime.
BEZEL_TEST(await_t_bang_pattern_b_5144,
           await_t_bang_second_await_rejected_spec_30_4) {
    auto r = parseAndCheckOwnership(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Mk(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Mk; "
        "  public static async method Main(): Task<int> "
        "    var t := C.Mk(); "
        "    var a := await t!; "
        "    var b := await t!; "    // second spend: §30.4 single-consumer
        "    a.Dispose(); "
        "    b.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "already awaited"));
}

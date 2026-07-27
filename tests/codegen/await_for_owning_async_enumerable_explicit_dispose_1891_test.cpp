// Codegen / runtime — issue #1891 (spec §29.2 / §30.7): an `await for` body
// that explicitly discharges its iter var (`c.Dispose()` / `_ := c`) must
// reclaim the element's heap block, exactly as the sync `for` sibling
// (`iterator_borrowing_collection_reclaim_2148_test.cpp`). The async owning
// iterator transfers the element into the for-slot through `TakeCurrent`;
// before the fix the destructor ran but the block never freed, so LSan
// reported 48 B / 3 objects (one per yielded Res). Reuses the established
// `runAotChecked` ASan/LSan oracle (every test below: leak under -O0
// would surface, but codegen defaults to the O2 sweep that masks it — the
// spec's exact wording: "a heap block is never freed").
//
// Spec authority:
//   - §29.1 v0.196: TakeCurrent transfers ownership; a Dispose() afterwards
//     disposes the still-owned element.
//   - §29.2: the iteration variable is owned; the §28.3 discharge rules
//     apply on every body-exit edge; explicit `c.Dispose()` and `_ := c`
//     are the borrowed-iter-var-free/discharge positions.
//   - §30.7: `await for` over IOwningAsyncEnumerable<T> desugars to
//     GetOwningAsyncEnumerator / MoveNextAsync / TakeCurrent.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Tracking resource: ctor/dtor on a static counter plus an Id field so each
// allocation is observable.
const char* kRes =
    "class Res "
    "  public static live: int := 0; "
    "  public static disposed: int := 0; "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; Res.live := Res.live + 1; end constructor; "
    "  destructor disposed := disposed + 1; end destructor; "
    "end Res; ";

const char* kGen =
    "class Gen "
    "  public static async method Produce(): IOwningAsyncEnumerable<Res> "
    "    yield return new Res(1); "
    "    yield return new Res(2); "
    "    yield return new Res(3); "
    "  end Produce; "
    "end Gen; ";

}  // namespace

// #1891: the exact issue repro — await-for + `c.Dispose()`. Each of the
// 3 yielded elements must be disposed exactly once (no leak, no double-
// free). Final observable: ctor count == dispose count.
BEZEL_TEST(await_for_owning_async_enumerable_explicit_dispose_1891,
           ctor_eq_dispose_spec_29_2_30_7) {
    const std::string src = std::string("module M; ") + kRes + kGen +
        "class C "
        "  public static async method Main(): Task<int> "
        "    await for var c in Gen.Produce() do "
        "      c.Dispose(); "
        "    end; "
        "    return Res.live - Res.disposed; "  // 0 iff balanced
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// #1891: same shape but discharged by the discard statement `_ := c;`.
// `_ := c` is the announced-consume discharge for the owned iteration
// variable (§17.13 / §30.5); every element's block must be reclaimed.
BEZEL_TEST(await_for_owning_async_enumerable_explicit_dispose_1891,
           discard_eq_dispose_spec_17_13_29_2) {
    const std::string src = std::string("module M; ") + kRes + kGen +
        "class C "
        "  public static async method Main(): Task<int> "
        "    await for var c in Gen.Produce() do "
        "      _ := c; "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// #1891: control — auto-drop path (no explicit discharge) was already
// correct before the fix. The unwinding use-var auto-dispose at body end
// reclaims every element. Verifies the fix did not regress the working
// path. Observable: ctor == dispose.
BEZEL_TEST(await_for_owning_async_enumerable_explicit_dispose_1891,
           auto_drop_unchanged_control_spec_29_2) {
    const std::string src = std::string("module M; ") + kRes + kGen +
        "class C "
        "  public static async method Main(): Task<int> "
        "    await for var c in Gen.Produce() do "
        "      using var _ := owned c; "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

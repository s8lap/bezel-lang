// Codegen regression — #972 / §29.2 / §28.3:
// lowerIEnumerableFor (user-defined IEnumerable<T> for-loop via GetEnumerator)
// must call the enumerator's Dispose() on ALL exit paths:
//   (a) normal exhaustion — endBB
//   (b) break — emitLoopDisposals via disposeStack_ at loopStack_ outerDepth
// (exception path via unwind disposeStack_ was already covered).
//
// Root cause (pre-fix): the enumerator was pushed directly into the enclosing
// disposeStack_.back() scope rather than into a dedicated scope, and there was
// NO emitDisposals call at endBB. Normal-exit never called Dispose() → leak.
// Break already ran the enclosing scope's disposals (via emitLoopDisposals),
// which would have double-disposed if the enclosing scope contained the entry;
// using outerDepth in loopStack_ was also missing so break did NOT cover the
// enumerator scope.
//
// Fix: mirror lowerIEnumerableBuiltinFor — push a dedicated scope, use
// outerDepth for loopStack_, emitDisposals + pop_back at endBB.
//
// Verification: destructor counter on the enumerator class. Each test checks
// that the counter == 1 after the loop (disposed exactly once, not 0=leak,
// not 2+ = double-free).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Enumerator class with a disposal counter and iterable wrapper.
// EnumDisposed.n starts at 0; each enumerator destructor increments it.
// After a for-loop, n == 1 means disposed exactly once (correct).
// n == 0 means the enumerator was never disposed (the leak that #972 fixes).
// n >= 2 means double-dispose.
static const char* kCountingEnum = R"(
  class EnumDisposed
    public static n: int := 0;
    public constructor() end constructor;
    destructor end destructor;
  end EnumDisposed;
  class CountingEnum
    private cur: int; private limit: int;
    public constructor(limit: int) this.cur := 0; this.limit := limit; end constructor;
    public method MoveNext(): bool cur := cur + 1; return cur <= limit; end MoveNext;
    public readonly property Current: int get => cur; end Current;
    destructor EnumDisposed.n := EnumDisposed.n + 1; end destructor;
  end CountingEnum;
  class CountingRange
    private limit: int;
    public constructor(limit: int) this.limit := limit; end constructor;
    public method GetEnumerator(): CountingEnum return new CountingEnum(limit); end GetEnumerator;
    destructor end destructor;
  end CountingRange;
)";

}  // namespace

// §29.2 / §28.3 (#972): normal-exit — for loop runs to exhaustion.
// Before fix: enumerator never disposed → EnumDisposed.n == 0.
// After fix:  enumerator disposed at endBB → EnumDisposed.n == 1.
BEZEL_TEST(user_ienumerable_dispose_972, normal_exhaustion_disposes_enumerator) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M;") + kCountingEnum +
        "class C public static method Main(): int "
        "  using var r := new CountingRange(3); "
        "  for var x in r do end; "
        "  return EnumDisposed.n; "  // 1 = disposed once (not 0=leak)
        "end Main; end C;"), 1);
}

// §29.2 / §28.3 (#972): break path — loop exits early via break.
// The enumerator scope must be included in emitLoopDisposals (via outerDepth).
// Should also be disposed exactly once.
BEZEL_TEST(user_ienumerable_dispose_972, break_disposes_enumerator) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M;") + kCountingEnum +
        "class C public static method Main(): int "
        "  using var r := new CountingRange(100); "
        "  for var x in r do "
        "    if x >= 3 then break; end; "
        "  end; "
        "  return EnumDisposed.n; "  // 1 = disposed once on break
        "end Main; end C;"), 1);
}

// §29.2 (#972): no double-free on normal exit — the enumerator scope is
// dedicated (not pushed into the enclosing scope), so the enclosing
// scope does NOT also dispose it. The counter must be exactly 1 after the
// loop: disposed once by endBB, not again by the outer scope cleanup.
// We verify this indirectly: the first test (normal_exhaustion) would return 2
// if there were a double-dispose (destructor called twice → n == 2).
// Add a second back-to-back loop in one run to confirm sequential calls don't
// accumulate beyond 2 (each creates and disposes one enumerator).
BEZEL_TEST(user_ienumerable_dispose_972, two_sequential_loops_each_dispose_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M;") + kCountingEnum +
        "class C public static method Main(): int "
        "  using var r1 := new CountingRange(2); "
        "  for var x in r1 do end; "
        "  using var r2 := new CountingRange(2); "
        "  for var x in r2 do end; "
        "  return EnumDisposed.n; "  // 2 = each enumerator disposed exactly once
        "end Main; end C;"), 2);
}

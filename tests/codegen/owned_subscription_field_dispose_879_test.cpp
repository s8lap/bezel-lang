// fix #879 / §28.5 / §24.4: an `owned` field of type Subscription (or
// Subscription?) must be auto-disposed (unsubscribed via bz_subscription_dispose)
// when the containing object is disposed.  emitOwnedFieldDisposals never
// classified a Subscription-typed owned field (Subscription is a compiler-known
// opaque handle with no user TypeDecl, not a class/exception/collection/Shared/
// Weak), so it was silently skipped — the handler kept firing after the containing
// object was disposed.  Fix: detect owned Subscription fields via
// isOwnedSubscriptionField and call bz_subscription_dispose (which is null-safe:
// `if (!subp) return;`) in the disposal loop, mirroring the Shared/Weak path.
//
// Value tests — the program returns the cumulative event total (raw int 0..255).
// If the subscription is disposed at containing-object scope-end, post-disposal
// Fire() calls are no-ops and the total stays at the pre-disposal value.
//
// AOT MIGRATION (JIT->AOT): runs under the AOT driver (runAot) — the stricter
// oracle that runs the full ownership + definite-assignment pipeline. Every
// expected value here is ≤255, so exit codes round-trip; no runAotOut needed.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Shared preamble: a Src with an event, a static Total accumulator.
const char* kPre =
    "module M; "
    "class Src "
    "  public event Ev: int; "
    "  public method Fire(v: int) Ev.Raise(v); end Fire; "
    "end Src; ";
}  // namespace

// §28.5 point 1 / §24.4: the basic repro (#879).  The owned Subscription field
// must be auto-disposed when Holder is disposed so Fire(50) is a no-op.
// Expected: Total = 5 (handler fired once inside the scope; not 55).
BEZEL_TEST(owned_subscription_field_dispose_879, basic_nonnullable_field) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class Holder "
        "  private owned _sub: Subscription; "
        "  public static Total: int := 0; "
        "  public constructor(src: Src) "
        "    this._sub := src.Ev.Subscribe((v) => do Holder.Total := Holder.Total + v; end); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var h := new Holder(src); "
        "    src.Fire(5); "
        "  end; "
        "  src.Fire(50); "
        "  return Holder.Total; "
        "end Main; end C;"), 5);
}

// §28.5 / §24.4: owned Subscription? (nullable) field holding a real subscription —
// bz_subscription_dispose is null-safe so it must unsubscribe cleanly.
// Expected: Total = 5 (handler fired once; Fire(50) is a no-op after disposal).
BEZEL_TEST(owned_subscription_field_dispose_879, nullable_field_assigned) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class Holder "
        "  private owned _sub: Subscription?; "
        "  public static Total: int := 0; "
        "  public constructor(src: Src) "
        "    this._sub := src.Ev.Subscribe((v) => do Holder.Total := Holder.Total + v; end); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var h := new Holder(src); "
        "    src.Fire(5); "
        "  end; "
        "  src.Fire(50); "
        "  return Holder.Total; "
        "end Main; end C;"), 5);
}

// §28.5 / §24.4: owned Subscription? field left null (never assigned) —
// disposal must be a no-op (no crash).  bz_subscription_dispose(null) returns
// immediately.  Expected: Total = 0, rc = 0.
BEZEL_TEST(owned_subscription_field_dispose_879, nullable_field_null_no_crash) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class Holder "
        "  private owned _sub: Subscription?; "
        "  public static Total: int := 0; "
        "  public constructor() end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var h := new Holder(); "
        "    src.Fire(5); "
        "  end; "
        "  src.Fire(50); "
        "  return Holder.Total; "
        "end Main; end C;"), 0);
}

// §28.5 / §24.4: two owned Subscription fields — both must be unsubscribed at
// containing-object disposal (reverse declaration order, per §28.5).
// Expected: Total = 5 (3+2, both fired once inside scope; post-scope is no-op).
BEZEL_TEST(owned_subscription_field_dispose_879, two_subscription_fields_both_unsubscribed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Src "
        "  public event EvA: int; "
        "  public event EvB: int; "
        "  public method FireA(v: int) EvA.Raise(v); end FireA; "
        "  public method FireB(v: int) EvB.Raise(v); end FireB; "
        "end Src; "
        "class Holder "
        "  private owned _subA: Subscription; "
        "  private owned _subB: Subscription; "
        "  public static Total: int := 0; "
        "  public constructor(src: Src) "
        "    this._subA := src.EvA.Subscribe((v) => do Holder.Total := Holder.Total + v; end); "
        "    this._subB := src.EvB.Subscribe((v) => do Holder.Total := Holder.Total + v; end); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var h := new Holder(src); "
        "    src.FireA(3); src.FireB(2); "
        "  end; "
        "  src.FireA(30); src.FireB(20); "
        "  return Holder.Total; "
        "end Main; end C;"), 5);
}

// §28.5 / §24.4: a Holder with both an owned Subscription field AND an owned
// class field — both must be disposed at scope-end, no crash.
// Expected: Total = 5 (Subscription disposed), Resource.Alive = 0 (class disposed).
// Return Total + Resource.Alive = 5.
BEZEL_TEST(owned_subscription_field_dispose_879, mixed_subscription_and_class_field) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class Resource "
        "  public static Alive: int := 0; "
        "  public constructor() Resource.Alive := Resource.Alive + 1; end constructor; "
        "  destructor Resource.Alive := Resource.Alive - 1; end destructor; "
        "end Resource; "
        "class Holder "
        "  private owned _sub: Subscription; "
        "  private owned _res: Resource; "
        "  public static Total: int := 0; "
        "  public constructor(src: Src) "
        "    this._sub := src.Ev.Subscribe((v) => do Holder.Total := Holder.Total + v; end); "
        "    this._res := new Resource(); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var h := new Holder(src); "
        "    src.Fire(5); "
        "  end; "
        "  src.Fire(50); "
        "  return Holder.Total + Resource.Alive; "
        "end Main; end C;"), 5);
}

// §24.4 regression: explicit `using var sub := …Subscribe(…)` still works after
// the fix (the local-variable path is unchanged).
// Expected: Total = 5 (sub disposed at inner scope-end; Fire(50) is a no-op).
BEZEL_TEST(owned_subscription_field_dispose_879, explicit_using_var_regression) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class Counter public static Total: int := 0; end Counter; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  do "
        "    using var sub := src.Ev.Subscribe((v) => do Counter.Total := Counter.Total + v; end); "
        "    src.Fire(5); "
        "  end; "
        "  src.Fire(50); "
        "  return Counter.Total; "
        "end Main; end C;"), 5);
}

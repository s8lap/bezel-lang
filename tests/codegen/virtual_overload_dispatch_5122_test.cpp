// Regression tests for #5122 (§22.6 / §12 / §12.1, v0.307): overloaded
// VIRTUAL methods sharing a name AND arity but differing in parameter types
// were assigned the SAME vtable slot (buildVtable keyed the slot by
// `name#arity` after #1085), so the second overload's slot overwrote the
// first's (last-writer-wins). Every base-typed-receiver call to the
// EARLIER-declared overload dispatched to the LATER-declared body's return
// value. The static-typed-receiver twin (`d.M(...)` on a `Der`-typed local)
// already worked because codegen directly invokes `Der.M(...)` without
// consulting the vtable slot.
//
// #1085 closed the arity-distinct case (same name, different arities);
// #5122 closes the residual case (same name + same arity, distinct param
// types). `buildVtable` now keys the slot by `methodSlotKey(ms)`, which
// appends each parameter's type spelling to the key (mirrors
// `indexerSlotKey` for #1080). Two virtuals `M(Foo)` and `M(int)` keyed
// `M#1#Foo.` and `M#1#int.` → distinct slots; a base virtual and its derived
// override of the same arity AND same param types keyed identically → the
// override reuses the base's slot.
//
// runAot returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) return -999;
    try {
        return std::stoi(r.stdoutText);
    } catch (...) {
        return r.exitCode;
    }
}

// Loop amplification (the §17 "one-shot leak" trap): a wrong-overload
// dispatch returns the LAST-declared body's value for every call, so summing
// across many iterations collapses into a single distinctive wrong value.
// 64 iterations is a power of two; the correct sum (10 * 64 = 640) and the
// wrong sum (20 * 64 = 1280) are far apart, with no off-by-one risk from
// the call count itself.
constexpr int kIters = 64;

}  // namespace

// ── T1 — HEADLINE repro: same-arity-different-param-types, base receiver ──
//
// Two virtual overloads `M(Foo)` and `M(int)` on `Base`. `b.M(new Foo())`
// (Foo arg, 1-arg arity) must dispatch to `Der.M(Foo)` returning 10 — NOT
// to the LAST-declared `Der.M(int)` returning 20 (the #5122 bug). Summed
// over 64 iterations the wrong value is 20 * 64 = 1280; the correct value
// is 10 * 64 = 640.
//
// Spec §22.6 / §12 / §12.1, v0.307.
BEZEL_TEST(virtual_overload_dispatch_5122,
           t1_base_typed_receiver_distinct_param_types_22_6_12_12_1) {
    const char* prog =
        "module Pph; "
        "public class Foo "
        "  public constructor() end constructor; "
        "end Foo; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method M(x: Foo): int return 1; end M; "
        "  public virtual method M(x: int): int return 2; end M; "
        "end Base; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method M(x: Foo): int return 10; end M; "
        "  public override method M(x: int): int return 20; end M; "
        "end Der; "
        "class App "
        "  public static method Main(): int "
        "    var d: Der := new Der(); "
        "    var b: Base := d; "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + b.M(new Foo()); "   // MUST be 10 each time
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "          // 640 post-fix; 1280 pre-fix
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 10 * kIters);
}

// And the int-arg form (the OTHER overload) must still reach its own body,
// not silently start hitting the wrong overload after the fix narrows by
// signature. Pre-fix it happened to reach 20 too (last-declared body
// under the colliding `M#1` key) — this row guards that the fix didn't
// *flip* to the wrong overload.
BEZEL_TEST(virtual_overload_dispatch_5122,
           t1_base_typed_receiver_int_form_22_6_12_12_1) {
    const char* prog =
        "module Pph; "
        "public class Foo "
        "  public constructor() end constructor; "
        "end Foo; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method M(x: Foo): int return 1; end M; "
        "  public virtual method M(x: int): int return 2; end M; "
        "end Base; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method M(x: Foo): int return 10; end M; "
        "  public override method M(x: int): int return 20; end M; "
        "end Der; "
        "class App "
        "  public static method Main(): int "
        "    var d: Der := new Der(); "
        "    var b: Base := d; "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + b.M(7); "           // MUST be 20 each time
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "          // 1280 (=20*64)
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 20 * kIters);
}

// ── T2 — POSITIVE control: same-arity-different-param-types, derived
//          receiver (static dispatch — already worked pre-fix) ────────────────
//
// A statically-dispatched call on a `Der`-typed receiver invokes `Der.M(Foo)`
// directly without consulting the vtable slot. The fix is scoped to
// `buildVtable`'s slot-key scheme; static dispatch is unchanged. Two
// overloads of `M` differ by parameter TYPE; `d.M(new Foo())` reaches the
// Foo body (10) and `d.M(7)` reaches the int body (20). No regression row.
BEZEL_TEST(virtual_overload_dispatch_5122,
           t2_derived_typed_receiver_distinct_param_types_22_6_12) {
    const char* prog =
        "module Pph; "
        "public class Foo "
        "  public constructor() end constructor; "
        "end Foo; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method M(x: Foo): int return 1; end M; "
        "  public virtual method M(x: int): int return 2; end M; "
        "end Base; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method M(x: Foo): int return 10; end M; "
        "  public override method M(x: int): int return 20; end M; "
        "end Der; "
        "class App "
        "  public static method Main(): int "
        "    var d: Der := new Der(); "
        "    if d.M(new Foo()) = 10 and d.M(7) = 20 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 1);
}

// ── T3 — REGRESSION guard: single-overload virtual override through a base
//          receiver — unchanged by the fix ────────────────────────────────────
//
// Single-overload classes keep their exact slot layout: distinct names
// produce distinct keys either way (the new methodSlotKey just APPENDS
// `#Foo.` etc., but a base `G(): int` and a derived `G(): int` both key
// the same `G#0#?` — actually `G#0#` for an empty param list; what matters
// is that the key is identical between base and derived override). The
// override must still dispatch correctly through the base receiver.
BEZEL_TEST(virtual_overload_dispatch_5122,
           t3_single_overload_override_through_base_22_6_12_12_1) {
    const char* prog =
        "module Pph; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method G(): int return 1; end G; "
        "end Base; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method G(): int return 9; end G; "
        "end Der; "
        "class App "
        "  public static method Main(): int "
        "    var d: Der := new Der(); "
        "    var b: Base := d; "
        "    Console.WriteLine(b.G()); "
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 9);
}
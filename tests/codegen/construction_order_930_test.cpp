// Codegen — §22.8 construction order: base/chained constructor → field
// initializers → constructor body. Issue #930.
//
// §22.8 (the v0.73 normalization) requires, for every constructor:
//   1. the base or chained constructor call,
//   2. field initializers in declaration order,
//   3. the constructor body.
// The compiler previously ran the whole hierarchy's field initializers at the
// `new` site, BEFORE any constructor (including the base chain) — the pre-v0.73
// order. So a derived field initializer ran while the base subobject was still
// zero-initialized (#930). The fix moves each class's OWN field initializers out
// of the `new`-site instance-init (now vtable-only) and into the constructor
// body after the base chain (and into the no-explicit-ctor construction
// sequence after the base sequence), so a derived field initializer observes the
// fully-constructed base subobject.
//
// (The sibling issue #929 — a primary constructor `Derived(v): Base(v)` invoking
// a base class's parameterized constructor — is a separate follow-up.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §22.8: a derived field initializer runs AFTER the base constructor body, so it
// observes a static the base constructor set (the #930 repro). Base ctor sets
// Seq.n := 99; Derived's `_snap := Seq.n` must capture 99, not the pre-ctor 0.
BEZEL_TEST(construction_order_930, derived_field_init_after_base_ctor_spec_22_8) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Seq public static n: int := 0; end Seq;"
        "public class Base public constructor() Seq.n := 99; end constructor;"
        "  destructor end destructor; end Base;"
        "public class Derived: Base"
        "  public _snap: int := Seq.n;"
        "  public constructor() base(); end constructor;"
        "end Derived;"
        "public class Program public static method Main(): int"
        "  using var d := new Derived(); return d._snap;"
        "end Main; end Program;"), 99);
}

// §22.8: with no explicit derived constructor, the implicit base() runs the base
// constructor before the derived field initializer (same ordering guarantee).
BEZEL_TEST(construction_order_930, no_ctor_derived_field_init_after_base_spec_22_8) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Seq public static n: int := 0; end Seq;"
        "public class Base public constructor() Seq.n := 7; end constructor;"
        "  destructor end destructor; end Base;"
        "public class Derived: Base public _snap: int := Seq.n; end Derived;"
        "public class Program public static method Main(): int"
        "  using var d := new Derived(); return d._snap;"
        "end Main; end Program;"), 7);
}

// §22.8: a plain (no-constructor) inheritance chain still initializes base and
// derived fields from their initializers — the construction sequence runs each
// level's own field initializers.
BEZEL_TEST(construction_order_930, no_ctor_base_and_derived_field_init) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Base public b: int := 10; end Base;"
        "class Derived: Base public d: int := 20; end Derived;"
        "class C public static method Main(): int"
        "  using var x := new Derived(); return x.b + x.d;"
        "end Main; end C;"), 30);
}

// §22.8: a three-level no-constructor chain initializes every level's own
// fields (1 + 2 + 4 = 7).
BEZEL_TEST(construction_order_930, three_level_no_ctor_field_init) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class A public a: int := 1; end A;"
        "class B: A public b: int := 2; end B;"
        "class D: B public d: int := 4; end D;"
        "class C public static method Main(): int"
        "  using var x := new D(); return x.a + x.b + x.d;"
        "end Main; end C;"), 7);
}

// §22.8 / §11.1: an explicit derived constructor's `base(v)` into a PRIMARY-ctor
// base runs the base's field initializer (`_field := v`) — the base's own field
// initializers run during the base chain, after its positional params are bound.
BEZEL_TEST(construction_order_930, explicit_base_call_into_primary_ctor_base) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base(v: int) public _field: int := v; end Base;"
        "public class ED: Base"
        "  public constructor(v: int) base(v); end constructor;"
        "end ED;"
        "class C public static method Main(): int"
        "  using var ed := new ED(7); return ed._field;"
        "end Main; end C;"), 7);
}

// §22.8 step 1: a `this(...)` delegation runs the chained sibling constructor's
// field initializers exactly once — the delegating constructor must NOT re-run
// them. The owned field `t` is allocated once (Tracker.live == 1 inside scope);
// a double-init would allocate two Trackers (one leaked) → live == 2.
BEZEL_TEST(construction_order_930, this_delegation_field_init_runs_once) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Tracker public static live: int := 0;"
        "  public constructor() Tracker.live := Tracker.live + 1; end constructor;"
        "  destructor Tracker.live := Tracker.live - 1; end destructor;"
        "end Tracker;"
        "class K"
        "  public owned t: Tracker := new Tracker();"
        "  public marker: int := 7;"
        "  public constructor() marker := 8; end constructor;"
        "  public constructor(x: int) this(); end constructor;"
        "end K;"
        "class C public static method Main(): int"
        "  using var k := new K(5); return Tracker.live;"
        "end Main; end C;"), 1);
}

// §22.9 / §28.3: a throwing derived field initializer disposes the
// fully-constructed base subobject AND the already-assigned derived owned field
// exactly once (no leak, no double-free). After the throw is caught, the
// destructor counter is back to 0 (base _bt and derived _dt both disposed once).
BEZEL_TEST(construction_order_930, throwing_field_init_disposes_base_once) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class AppError: Exception"
        "  public constructor() base(\"boom\"); end constructor; end AppError;"
        "class Tracker public static live: int := 0;"
        "  public constructor() Tracker.live := Tracker.live + 1; end constructor;"
        "  destructor Tracker.live := Tracker.live - 1; end destructor;"
        "end Tracker;"
        "class Boom public constructor() throw new AppError(); end constructor; end Boom;"
        "class Base"
        "  public owned _bt: Tracker;"
        "  public constructor() _bt := new Tracker(); end constructor;"
        "end Base;"
        "class Derived: Base"
        "  public owned _dt: Tracker := new Tracker();"
        "  public owned _boom: Boom := new Boom();"
        "  public constructor() base(); end constructor;"
        "end Derived;"
        "class C public static method Main(): int"
        "  try using var d := new Derived();"
        "  catch AppError => return Tracker.live; end;"
        "  return 99;"
        "end Main; end C;"), 0);
}

// §11.1 (#929): a primary constructor `Derived(v): Base(v)` invokes the base
// class's PARAMETERIZED constructor (running its body), not just pre-populating
// positional fields. Base.constructor(x) sets `_x := x`, so `new Derived(42)`
// gives `_x == 42` (previously 0 — the base ctor was never called).
BEZEL_TEST(construction_order_929, primary_ctor_calls_parameterized_base_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base public _x: int;"
        "  public constructor(x: int) _x := x; end constructor;"
        "  destructor end destructor;"
        "end Base;"
        "public class Derived(v: int): Base(v) end Derived;"
        "public class Program public static method Main(): int"
        "  using var d := new Derived(42); return d._x;"
        "end Main; end Program;"), 42);
}

// §11.1 (#929): a multi-parameter base constructor invoked from a primary-ctor
// derived class — `Derived(x, y): Base(x, y)` runs `Base.constructor(a, b)`.
BEZEL_TEST(construction_order_929, primary_ctor_multi_param_base_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base public _a: int; public _b: int;"
        "  public constructor(a: int, b: int) _a := a; _b := b; end constructor;"
        "end Base;"
        "public class Derived(x: int, y: int): Base(x, y) end Derived;"
        "class C public static method Main(): int"
        "  using var d := new Derived(30, 12); return d._a + d._b;"
        "end Main; end C;"), 42);
}

// §11.1 (#929): the inheritance-clause argument may be an expression over the
// primary-ctor parameter — `Base(v + 2)` runs `Base.constructor(40 + 2)`.
BEZEL_TEST(construction_order_929, primary_ctor_base_arg_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Base public _x: int;"
        "  public constructor(x: int) _x := x; end constructor; end Base;"
        "public class Derived(v: int): Base(v + 2) end Derived;"
        "class C public static method Main(): int"
        "  using var d := new Derived(40); return d._x;"
        "end Main; end C;"), 42);
}

// §11.1 / §28.3 (#929): a base constructor invoked via the primary-ctor chain
// that allocates an owned field is disposed exactly once at scope end (no leak,
// no double-free). The base ctor allocates a Tracker; after the scope the
// destructor counter is back to 0.
BEZEL_TEST(construction_order_929, primary_ctor_base_owned_field_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Tracker public static live: int := 0;"
        "  public constructor() Tracker.live := Tracker.live + 1; end constructor;"
        "  destructor Tracker.live := Tracker.live - 1; end destructor;"
        "end Tracker;"
        "public class Base public owned _t: Tracker; public _v: int;"
        "  public constructor(v: int) _t := new Tracker(); _v := v; end constructor;"
        "end Base;"
        "public class Derived(v: int): Base(v) end Derived;"
        "class C public static method Main(): int"
        "  do using var d := new Derived(5); end; return Tracker.live;"
        "end Main; end C;"), 0);
}

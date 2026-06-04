// Codegen — generic monomorphization / type-substitution hardening (issue #361).
//
// Two substitution gaps in generic codegen:
//
//   D12 — `new T()` inside a generic body never called T's constructor. Per
//   §14.2: "For a *class* substitution, `new T()` produces an owned reference
//   via the class's parameterless constructor"; "Inside a generic body,
//   `new T()` for a *record* substitution is equivalent to `default(T)` — the
//   parameterless constructor produces the zero-valued record." The bare
//   type-parameter TypeRef (no declaration) fell through every construction
//   branch in lowerNew, so the constructor count was 0 / the result was garbage.
//
//   D13 — `b.v.x` where `v: T` on `Holder<T>` and T=record read garbage. Per
//   §14.1: a field declared with a generic-parameter type, accessed off a
//   concrete instantiation, has the substituted type — `b: Holder<Inner>`
//   makes `b.v` an `Inner`, so `.x` resolves against Inner's layout. Sema typed
//   `b.v` as the bare unresolved `T`, so the trailing `.x` could not resolve.
//
// Bezel has no explicit call-site type-argument syntax (the grammar's
// PrimarySuffix has no `<...>` form; §A.7); a generic method's type parameters
// are inferred from its arguments (§26.4: "the same unification rules as
// generic method type inference"). The tests below therefore drive T through
// the receiver's class type argument and through value arguments, never through
// an (unsupported) explicit `Method<T>()` form.
//
// All tests are load-bearing: each FAILED before the fix (constructor count 0,
// or garbage field read) and passes after. Only real stdlib surface is used;
// observable resource counting uses a static field bumped by a constructor /
// destructor (the pattern used across the codegen suite).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── D12: `new T()` calls the constructor at the monomorphized instantiation ──

// §14.2: "For a *class* substitution, `new T()` produces an owned reference via
// the class's parameterless constructor." The instance generic method
// `Maker<Widget>.Make()` returns `new T()`; with T=Widget the parameterless
// constructor must run exactly once, so the static counter is 1.
// Before the fix: 0 (constructor never called).
BEZEL_TEST(generic_mono_361, new_t_class_instance_method_calls_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class Maker<T> where T: class, new() "
        "  public constructor() end constructor; "
        "  public method Make(): T return new T(); end Make; "
        "  destructor end destructor; "
        "end Maker; "
        "class C public static method Main(): int "
        "  using var m := new Maker<Widget>(); "
        "  using var w := m.Make(); "
        "  return Widget._n; "       // exactly one ctor call
        "end Main; end C;"), 1);
}

// §14.2: same rule for a *static* generic method, with T inferred from a value
// argument (`proto: T`). `Clone(p)` constructs a fresh T via `new T()` while
// `p` was already constructed — so the counter is 2.
// Before the fix: 1 (the `new T()` constructed nothing).
BEZEL_TEST(generic_mono_361, new_t_class_static_method_calls_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "static class Factory "
        "  public static method Clone<T>(proto: T): T where T: class, new() "
        "    return new T(); "
        "  end Clone; "
        "end Factory; "
        "class C public static method Main(): int "
        "  using var p := new Widget(); "
        "  using var w := Factory.Clone(p); "
        "  return Widget._n; "       // p's ctor + new T()'s ctor
        "end Main; end C;"), 2);
}

// §14.2: "Inside a generic body, `new T()` for a *record* substitution is
// equivalent to `default(T)` — the parameterless constructor produces the
// zero-valued record." With T=Point, `new T()` is Point(0, 0); X+Y+7 = 7.
// Before the fix: garbage (the record was never built).
BEZEL_TEST(generic_mono_361, new_t_record_substitution_is_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class Maker<T> where T: value, new() "
        "  public constructor() end constructor; "
        "  public method Make(): T return new T(); end Make; "
        "  destructor end destructor; "
        "end Maker; "
        "class C public static method Main(): int "
        "  using var m := new Maker<Point>(); "
        "  var p: Point := m.Make(); "
        "  return p.X + p.Y + 7; "   // default(Point) = (0,0)
        "end Main; end C;"), 7);
}

// §14.2: `new T()` is also called when it appears as a local-variable
// initializer inside the monomorphized body (a different lowering path than
// `return new T()`). The generic body names no concrete type; constructing
// twice bumps Widget._n to 2.
// Before the fix neither `new T()` constructed anything, so _n stayed 0.
BEZEL_TEST(generic_mono_361, new_t_local_initializer_calls_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class Maker<T> where T: class, new() "
        "  public constructor() end constructor; "
        "  public method MakeTwo() "
        "    using var a := new T(); "
        "    using var b := new T(); "
        "  end MakeTwo; "
        "  destructor end destructor; "
        "end Maker; "
        "class C public static method Main(): int "
        "  using var m := new Maker<Widget>(); "
        "  m.MakeTwo(); "
        "  return Widget._n; "  // two `new T()` → 2
        "end Main; end C;"), 2);
}

// ── D13: chained field access through a generic record field ──────────────────

// §14.1: a field `v: T` on `Holder<T>`, accessed off `b: Holder<Inner>`, has
// type `Inner`; the chained `b.v.x` reads Inner.x (= 42). The receiver is a
// borrow parameter so the read reaches codegen (a `using var` source would
// otherwise raise the §28.3 borrow-lifetime rule on the chained access).
// Before the fix: 0 (the `.x` field load was not substituted to Inner's layout).
BEZEL_TEST(generic_mono_361, chained_field_through_generic_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(x: int, y: int); "
        "class Holder<T> "
        "  public v: T; "
        "  public constructor(val: T) v := val; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method ReadX(b: Holder<Inner>): int return b.v.x; end ReadX; "
        "  public static method Main(): int "
        "    using var b := new Holder<Inner>(new Inner(42, 7)); "
        "    return ReadX(b); "
        "  end Main; "
        "end C;"), 42);
}

// §14.1: the trailing field selects the right slot (second positional field
// y = 7), confirming the substituted layout is correct, not just nonzero.
BEZEL_TEST(generic_mono_361, chained_field_selects_correct_slot) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(x: int, y: int); "
        "class Holder<T> "
        "  public v: T; "
        "  public constructor(val: T) v := val; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method ReadY(b: Holder<Inner>): int return b.v.y; end ReadY; "
        "  public static method Main(): int "
        "    using var b := new Holder<Inner>(new Inner(42, 7)); "
        "    return ReadY(b); "
        "  end Main; "
        "end C;"), 7);
}

// §14.1: the chained access works directly off the `using var` source too. The
// access yields an `int` value (not a borrow of `b`), so the §28.3 borrow check
// does not fire — this also exercises that `b.v.x` now types as `int`.
// Before the fix this was both a sema false positive and a garbage read.
BEZEL_TEST(generic_mono_361, chained_field_on_using_var_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(x: int, y: int); "
        "class Holder<T> "
        "  public v: T; "
        "  public constructor(val: T) v := val; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var b := new Holder<Inner>(new Inner(42, 7)); "
        "  var result: int := b.v.x; "
        "  return result; "
        "end Main; end C;"), 42);
}

// §14.1: the typed-local workaround (`var p: T := b.v; p.x`) must keep working
// after the fix — it was the only path that read the field correctly before.
BEZEL_TEST(generic_mono_361, generic_record_field_typed_local_workaround) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(x: int, y: int); "
        "class Holder<T> "
        "  public v: T; "
        "  public constructor(val: T) v := val; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method ReadX(b: Holder<Inner>): int "
        "    var p: Inner := b.v; "
        "    return p.x; "
        "  end ReadX; "
        "  public static method Main(): int "
        "    using var b := new Holder<Inner>(new Inner(42, 7)); "
        "    return ReadX(b); "
        "  end Main; "
        "end C;"), 42);
}

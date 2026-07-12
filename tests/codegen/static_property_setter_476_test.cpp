// Codegen bug #476 — static property setter was a silent no-op (§23.3 / §13).
//
// An assignment `X := v` to a static property must call the setter body with
// `value` bound to v, with no `this` receiver (static context). The instance
// property path (CG49) uses the receiver; the static path must bypass the
// recv setup entirely and run the body directly — mirroring the static getter
// path in LlvmBackend_Exprs.cpp (the "isStaticProp" branch there).
//
// Tests:
//   1. Repro: side-effecting setter increments a counter → _calls == 1.
//   2. Round-trip: `X := 7; return X;` → 7.
//   3. Cross-class: `Cfg.Level := 3` set from another class reads back 3.
//   4. Clamping setter (validation logic) runs for static property.
//   5. Regression: instance property setter still works.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §23.3 / #476 test 1: repro — the setter body must run (side-effect counter).
// After `X := 5`, _calls should be 1 (not 0).
BEZEL_TEST(codegen_bug476, static_setter_side_effect_counter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 0; "
        "  public static _calls: int := 0; "
        "  public static property X: int "
        "    get => _x; "
        "    set => do _calls := _calls + 1; _x := value; end; "
        "  end X; "
        "  public static method Main(): int "
        "    X := 5; "
        "    return _calls; "
        "  end Main; "
        "end C;"), 1);
}

// §23.3 / #476 test 2: backing field is updated (_x == 5 after `X := 5`).
BEZEL_TEST(codegen_bug476, static_setter_updates_backing_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 0; "
        "  public static _calls: int := 0; "
        "  public static property X: int "
        "    get => _x; "
        "    set => do _calls := _calls + 1; _x := value; end; "
        "  end X; "
        "  public static method Main(): int "
        "    X := 5; "
        "    return _x; "
        "  end Main; "
        "end C;"), 5);
}

// §23.3 / #476 test 3: round-trip — `X := 7; return X;` → 7.
BEZEL_TEST(codegen_bug476, static_setter_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _x: int := 0; "
        "  public static property X: int "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end X; "
        "  public static method Main(): int "
        "    X := 7; "
        "    return X; "
        "  end Main; "
        "end C;"), 7);
}

// §23.3 / §13 / #476 test 4: cross-class static property set via `Cfg.Level := 3`.
BEZEL_TEST(codegen_bug476, cross_class_static_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cfg "
        "  public static _level: int := 0; "
        "  public static property Level: int "
        "    get => _level; "
        "    set => do _level := value; end; "
        "  end Level; "
        "end Cfg; "
        "class App "
        "  public static method Main(): int "
        "    Cfg.Level := 3; "
        "    return Cfg.Level; "
        "  end Main; "
        "end App;"), 3);
}

// §23.3 / #476 test 5: clamping setter — validation logic inside setter runs.
// Setter clamps to [0..100]; assigning 150 → stored as 100.
BEZEL_TEST(codegen_bug476, static_setter_with_clamping_logic) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _v: int := 0; "
        "  public static property V: int "
        "    get => _v; "
        "    set => do "
        "      if value > 100 then _v := 100; else _v := value; end; "
        "    end; "
        "  end V; "
        "  public static method Main(): int "
        "    V := 150; "
        "    return V; "
        "  end Main; "
        "end C;"), 100);
}

// §23.3 / #476 test 6: regression — instance property setter still works (CG49).
BEZEL_TEST(codegen_bug476, instance_setter_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end P; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 42; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

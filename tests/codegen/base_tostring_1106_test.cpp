// Regression tests for #1106 part L1b (§12.3 / §8.5): `base.ToString()`.
//
// `base.ToString()` inside an overriding `ToString()` must dispatch the BASE
// class's implementation NON-virtually (§12.3) — never the virtual vtable slot,
// which points at the most-derived override and recurses infinitely
// (base.ToString() inside the override → the same override → stack-overflow
// SIGSEGV). When no ancestor overrides ToString (base is Object), it returns the
// §8.5 type-name default — Object.ToString returns the runtime type's name,
// i.e. the enclosing class's name. Found by adversarial discovery sweep #9
// (#1106 defect 5).
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1106/5: base.ToString() on a class with no explicit base (base is Object,
// no override) returns the type-name default — the enclosing class name (was a
// stack-overflow SIGSEGV from infinite virtual self-dispatch).
BEZEL_TEST(base_tostring_1106, no_base_override_typename_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Aa public override method ToString(): string => base.ToString(); end Aa; "
        "class C public static method Main(): int "
        "  using var a := new Aa(); var s := a.ToString(); "
        "  return s.Length; "                              // "Aa" → 2
        "end Main; end C;"), 2);
}

// #1106/5: base.ToString() where the EXPLICIT base class overrides ToString
// dispatches the base's override (non-virtual), not the most-derived one.
BEZEL_TEST(base_tostring_1106, base_override_dispatched) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public virtual method ToString(): string => \"BASE\"; end B; "
        "class A: B public override method ToString(): string => base.ToString(); end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); var s := a.ToString(); "
        "  return s.Length; "                              // "BASE" → 4
        "end Main; end C;"), 4);
}

// #1106/5: an explicit base with NO ToString override → type-name default of
// the enclosing (runtime) class, not the base.
BEZEL_TEST(base_tostring_1106, explicit_base_no_override_typename) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B end B; "
        "class Abc: B public override method ToString(): string => base.ToString(); end Abc; "
        "class C public static method Main(): int "
        "  using var a := new Abc(); var s := a.ToString(); "
        "  return s.Length; "                              // "Abc" (runtime type) → 3
        "end Main; end C;"), 3);
}

// Regression guard: a plain (non-base) overriding ToString() is unchanged.
BEZEL_TEST(base_tostring_1106, plain_override_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class A public override method ToString(): string => \"hello\"; end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); var s := a.ToString(); "
        "  return s.Length; "                              // "hello" → 5
        "end Main; end C;"), 5);
}

// Regression guard: the default ToString (no override) on a plain class still
// returns a non-empty type-name.
BEZEL_TEST(base_tostring_1106, default_tostring_nonempty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget public _v: int := 0; end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); var s := w.ToString(); "
        "  return (1 if s.Length > 0 else 0); "            // "Widget" → 1
        "end Main; end C;"), 1);
}

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

// #2934 (spec §8.5, v0.252): the expectation(s) marked below changed value
// when §8.5's *name-only* `ToString` defaults were pinned to stdlib §2.13's
// `Type.FullName` rendering ("the module-path-qualified rendering stdlib
// §2.13 defines for `Type.FullName`"). Only the three name-only defaults
// moved — classes, enums, and body records that do NOT satisfy §9.2's
// auto-equality conditions. Record HEADS are unchanged (§8.5: "positional-
// record and auto-equality formatting keeps the **unqualified** head"), and
// so is every user `ToString` override.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
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
        "  return s.Length; "                              // #2934 (§8.5, v0.252):
        "end Main; end C;"), 4);   // was "Aa" (2); now "M.Aa" (4)
}

// #1106/5: base.ToString() where the EXPLICIT base class overrides ToString
// dispatches the base's override (non-virtual), not the most-derived one.
BEZEL_TEST(base_tostring_1106, base_override_dispatched) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public override method ToString(): string => \"BASE\"; end B; "
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
        "  return s.Length; "                              // #2934 (§8.5, v0.252):
        "end Main; end C;"), 5);   // was "Abc" (3); now "M.Abc" (5)
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
        "  return (if s.Length > 0 then 1 else 0); "            // "Widget" → 1
        "end Main; end C;"), 1);
}

// #1110 (§4.6 / §8.5 / §12.3): a `base`-qualified `ToString()` call used
// directly as a string-`+` operand must type as `string`. When the class has
// NO explicit class base, `base` is `Object`, so the call had no resolvable
// receiver TypeSymbol and was left untyped — `"x" + base.ToString()` then saw a
// non-`string` right operand and was rejected by the §4.6 concat-operand check
// even though `base.ToString()` (Object.ToString → the runtime type name)
// yields a string. The result is "x" + "Aa" = "xAa" (length 3).
BEZEL_TEST(base_tostring_1110, concat_operand_object_base) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Aa public override method ToString(): string => \"x\" + base.ToString(); end Aa; "
        "class C public static method Main(): int "
        "  using var a := new Aa(); var s := a.ToString(); "
        "  return s.Length; "                              // #2934 (§8.5, v0.252):
        "end Main; end C;"), 5);   // was "xAa" (3); now "xM.Aa" (5)
}

// #1110 (§4.6 / §8.5 / §12.3): the same for the LEFT `+` operand position,
// `base.ToString() + "!"` — also rejected before the fix. "Aa" + "!" → 3.
BEZEL_TEST(base_tostring_1110, concat_left_operand_object_base) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Aa public override method ToString(): string => base.ToString() + \"!\"; end Aa; "
        "class C public static method Main(): int "
        "  using var a := new Aa(); var s := a.ToString(); "
        "  return s.Length; "                              // #2934 (§8.5, v0.252):
        "end Main; end C;"), 5);   // was "Aa!" (3); now "M.Aa!" (5)
}

// #1110 (§4.6 / §8.5 / §12.3): an EXPLICIT class base whose override returns a
// fixed string concatenated as a `+` operand. (This path already typed
// correctly via the user-class member-access typing, but guard it against
// regression alongside the Object-base fix.) "x" + "BASE" → 5.
BEZEL_TEST(base_tostring_1110, concat_operand_explicit_base) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public override method ToString(): string => \"BASE\"; end B; "
        "class A: B public override method ToString(): string => \"x\" + base.ToString(); end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); var s := a.ToString(); "
        "  return s.Length; "                              // "xBASE" → 5
        "end Main; end C;"), 5);
}

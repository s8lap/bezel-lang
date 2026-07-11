// Codegen slice CG34 — non-destructive record update `with` (§9.5) and its
// retired class-source sibling.
//
// Before v0.188 (proposal 0037 graduation, #1735) `with` on a CLASS source
// was a compile-time-sugared "object initializer block" (former §22.11):
// `new C() with f := v, … end` constructed the instance, then assigned each
// named field/auto-property in place through its setter, returning the same
// pointer. §9.5 (v0.188) retired that reading: a `with` source must be
// record-typed, and a class-typed source — `new` or not — is now a compile
// error naming the rule (see with_expression_target_validation_875_test.cpp
// for the rejection + fix-it coverage). The tests below, which exercised the
// class form, are migrated to the diagnostic's OWN second remedy — bind the
// instance to a named local, assign its members as ordinary statements —
// which observably behaves the same way the retired sugar did (same
// destructor-count / value outcomes), because that is exactly what the sugar
// desugared to. See tests/codegen/objinit_usingvar_disposal_test.cpp for the
// disposal-focused migration of the same construct.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §9.5: record `with` — the surviving form — still sets fields via a
// non-destructive copy.
BEZEL_TEST(codegen_cg34, with_sets_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(_x: int, _y: int); "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(0, 0) with _x := 40, _y := 2 end; "
        "    return r._x + r._y; "
        "  end Main; "
        "end C;"), 42);
}

// §22.5 remedy (migrated from the retired class-source form, #1735): bind
// the instance to a local and assign its fields as statements. The result
// keeps its declared type, so a method call on it still resolves.
BEZEL_TEST(codegen_cg34, with_migrated_result_keeps_type_for_method_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; public _y: int := 0; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var c := new C(); c._x := 30; c._y := 12; "
        "    return c.Sum(); "
        "  end Main; "
        "end C;"), 42);
}

// §22.5 remedy (migrated, #1735): a field initialized to 5, then overridden
// to 7 by an ordinary assignment (was: `with _v := 7 end`).
BEZEL_TEST(codegen_cg34, with_migrated_overrides_initializer) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _v: int := 5; "
        "  public static method Main(): int "
        "    var c := new C(); c._v := 7; return c._v; "
        "  end Main; "
        "end C;"), 7);
}

// §22.5 remedy (migrated, #1735): an auto-property assigned through its
// setter (was: `with P := 42 end`).
BEZEL_TEST(codegen_cg34, with_migrated_sets_auto_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property P: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 42; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// fix #747 R43 (pre-v0.188) established that if a setter (or a setter's
// value expression) throws during a CLASS object-initializer block, the
// partially-initialized instance is disposed by an implicit try/finally
// before the exception propagates. §9.5/v0.188 retired the class-source
// `with` form entirely, and with it the UNATTACHED-owned landing pad #747
// R43 needed: the §22.5-remedy form (bind to a `using var` FIRST, THEN
// assign members) never has an unattached intermediate — the instance is a
// fully-bound owned local before any member assignment runs, so an ordinary
// throwing assignment is covered by the general owned-local exception
// discharge (§28.3/§28.10), not by any with-specific mechanism. These tests,
// migrated to that two-statement form, now pin exactly that general
// guarantee — the guarantee the retired construct used to restate (#1735).
// All exits are caught, so the JIT process never aborts; exit codes are <
// 256 (no truncation).
// ---------------------------------------------------------------------------

// A field's value expression throws → the instance's destructor still runs
// (W.disposed → 1) via the ordinary `using var` discharge on unwind.
BEZEL_TEST(codegen_cg34, using_var_field_assign_throws_disposes_instance_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public X: int := 0; "
        "  public static disposed: int := 0; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C "
        "  public static method Bang(): int throw new Exception(\"boom\"); end Bang; "
        "  public static method Main(): int "
        "    try "
        "      using var w := new W(); "
        "      w.X := C.Bang(); "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// A later field assignment throws after an earlier one stored (the instance
// is still disposed exactly once; preceding assignments are not rolled
// back — same observable behavior §22.11 used to specify for its sugar).
BEZEL_TEST(codegen_cg34, using_var_second_field_assign_throws_disposes_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public A: int := 0; public B: int := 0; "
        "  public static disposed: int := 0; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C "
        "  public static method Bang(): int throw new Exception(\"boom\"); end Bang; "
        "  public static method Main(): int "
        "    try "
        "      using var w := new W(); "
        "      w.A := 10; w.B := C.Bang(); "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// SAFETY (must not double-dispose): a successful two-statement construction
// disposes the instance EXACTLY ONCE at scope end.
BEZEL_TEST(codegen_cg34, using_var_success_disposes_once_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public X: int := 0; "
        "  public static disposed: int := 0; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C public static method Main(): int "
        "  do using var w := new W(); w.X := 5; end; "
        "  return W.disposed; "  // 1, not 2
        "end Main; end C;"), 1);
}

// SAFETY (must not double-dispose): a successful construction followed by a
// LATER throw while the instance is bound disposes it exactly once.
BEZEL_TEST(codegen_cg34, using_var_success_then_later_throw_disposes_once_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public X: int := 0; "
        "  public static disposed: int := 0; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C "
        "  public static method Bang(): int throw new Exception(\"boom\"); end Bang; "
        "  public static method Main(): int "
        "    try "
        "      using var w := new W(); w.X := 5; "
        "      var z := C.Bang(); "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// A custom (block) setter runs its body via ordinary assignment (was: `with
// V := 5 end` on a class source, #747 R43). The setter body runs and
// transforms the value (doubles it): V := 5 → 10.
BEZEL_TEST(codegen_cg34, using_var_runs_custom_setter_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  private _v: int := 0; "
        "  public property V: int get => _v; set => do _v := value * 2; end; end; "
        "end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); w.V := 5; "
        "  return w.V; "  // 10 — the setter ran
        "end Main; end C;"), 10);
}

// A custom setter that throws propagates out of the assignment.
BEZEL_TEST(codegen_cg34, using_var_setter_throw_propagates_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  private _v: int := 0; "
        "  public property V: int get => _v; set => do "
        "    if value >= 5 then throw new InvalidOperationException(\"big\"); end; "
        "    _v := value; "
        "  end; end; "
        "end W; "
        "class C public static method Main(): int "
        "  try "
        "    using var w := new W(); w.V := 5; "
        "    return 0; "
        "  catch InvalidOperationException => return 1; end; "  // 1
        "end Main; end C;"), 1);
}

// §28.3/§28.10: when the custom setter throws, the already-bound instance is
// also disposed by the `using var` discharge before the exception
// propagates.
BEZEL_TEST(codegen_cg34, using_var_setter_throw_disposes_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  private _v: int := 0; "
        "  public static disposed: int := 0; "
        "  public property V: int get => _v; set => do "
        "    if value >= 5 then throw new InvalidOperationException(\"big\"); end; "
        "    _v := value; "
        "  end; end; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C public static method Main(): int "
        "  try "
        "    using var w := new W(); w.V := 5; "
        "    return 99; "
        "  catch InvalidOperationException => return W.disposed; end; "  // 1
        "end Main; end C;"), 1);
}

// CONTROL: an auto-property target still works via ordinary assignment (its
// setter is a plain field store; the custom-setter path above must not
// change it).
BEZEL_TEST(codegen_cg34, using_var_auto_property_unregressed_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W public property P: int; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); w.P := 42; "
        "  return w.P; "  // 42
        "end Main; end C;"), 42);
}

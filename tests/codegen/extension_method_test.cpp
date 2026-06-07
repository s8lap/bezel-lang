// Codegen tests for extension methods (§26, issue #433).
//
// §26.1: inside an extension block `this` is the receiver. §26.5: when a
// member access `x.M(...)` finds no instance member, the compiler resolves M
// against an extension declared on x's type; an instance member of the same
// name always wins (no shadow). These tests JIT-run programs that call
// extension methods and assert the integer the program returns.
//
// The §26.1 codegen contract verified here: the receiver `this` is passed by
// VALUE for a primitive/value receiver (e.g. i32 for `extension int` — the
// historical malformed-IR crash, now fixed) and by POINTER for a class
// receiver (the object pointer, exactly as a normal method).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel.
int runMod(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ===========================================================================
// §26.1 / §26.5 — extension method on a CLASS receiver. `this` is the object
// pointer; `this._x` reads a field, exactly as a normal method body would.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_class_reads_field_spec_26_1) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "end Foo; "
        "extension Foo "
        "  public method Bar(): int => this._x + 1; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(41); "
        "  return f.Bar(); "
        "end Main; end P;"), 42);
}

// §26.5: an extension method that takes an argument in addition to `this`.
BEZEL_TEST(codegen_extension, method_on_class_with_arg_spec_26_5) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "end Foo; "
        "extension Foo "
        "  public method AddN(n: int): int => this._x + n; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(40); "
        "  return f.AddN(2); "
        "end Main; end P;"), 42);
}

// ===========================================================================
// §26.1 / §26.2 — extension method on a PRIMITIVE receiver (`extension int`).
// This was the malformed-IR crash: `this` lowered as a pointer for a value
// receiver. `this` is now the i32 VALUE. `(5).Double()` and `x.Double()`.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_int_literal_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class P public static method Main(): int "
        "  return (5).Double(); "
        "end Main; end P;"), 10);
}

BEZEL_TEST(codegen_extension, method_on_int_local_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Triple(): int => this * 3; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 7; "
        "  return x.Triple(); "
        "end Main; end P;"), 21);
}

// §26.5: a primitive extension method that takes an argument.
BEZEL_TEST(codegen_extension, method_on_int_with_arg) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Plus(n: int): int => this + n; "
        "end; "
        "class P public static method Main(): int "
        "  var x := 40; "
        "  return x.Plus(2); "
        "end Main; end P;"), 42);
}

// ===========================================================================
// §26.2 / §26.1 — extension method on a RECORD receiver (value type). `this`
// is a pointer to a spilled copy of the record value, so `this.field` reuses
// the record field-access machinery.
// ===========================================================================
BEZEL_TEST(codegen_extension, method_on_record_value_receiver_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Point(x: int, y: int); "
        "extension Point "
        "  public method Sum(): int => this.x + this.y; "
        "end; "
        "class P public static method Main(): int "
        "  var pt := new Point(40, 2); "
        "  return pt.Sum(); "
        "end Main; end P;"), 42);
}

// §26.2 — extension method on an ENUM receiver (value type, i32 `this`).
BEZEL_TEST(codegen_extension, method_on_enum_value_receiver_spec_26_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "extension Color "
        "  public method Code(): int => 7; "
        "end; "
        "class P public static method Main(): int "
        "  var c := Color.Green; "
        "  return c.Code(); "
        "end Main; end P;"), 7);
}

// ===========================================================================
// §26.5 NO-SHADOW — "Adding a real method to a type silently takes priority
// over an extension with the same name — by design." The INSTANCE member must
// win; the extension's same-named method must NOT be selected.
// ===========================================================================
BEZEL_TEST(codegen_extension, instance_member_wins_over_extension_spec_26_5) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public method Value(): int => 100; "
        "end Foo; "
        "extension Foo "
        "  public method Value(): int => 1; "
        "end; "
        "class P public static method Main(): int "
        "  var f := new Foo(); "
        "  return f.Value(); "   // instance 100 wins, not extension 1
        "end Main; end P;"), 100);
}

// ===========================================================================
// REGRESSION — a program with NO extensions is unaffected (a class method
// call resolves and runs exactly as before).
// ===========================================================================
BEZEL_TEST(codegen_extension, no_extension_program_unaffected) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Foo "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "  public method Get(): int => _x; "
        "end Foo; "
        "class P public static method Main(): int "
        "  var f := new Foo(42); "
        "  return f.Get(); "
        "end Main; end P;"), 42);
}

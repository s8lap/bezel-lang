// Codegen slice CG34 — object initializer `new C() with f := v, …`
// (§A.7). The source is constructed, then each named field/auto-property
// is assigned. The result has the source's type. Observable via the int
// exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg34, with_sets_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; public _y: int := 0; "
        "  public static method Main(): int "
        "    var c := new C() with _x := 40, _y := 2 end; "
        "    return c._x + c._y; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg34, with_result_keeps_type_for_method_call) {
    // The `with` result is typed as C, so a method call on it resolves.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; public _y: int := 0; "
        "  public method Sum(): int => _x + _y; "
        "  public static method Main(): int "
        "    var c := new C() with _x := 30, _y := 12 end; "
        "    return c.Sum(); "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg34, with_overrides_initializer) {
    // A field initialized to 5 is overridden to 7 by the `with`.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _v: int := 5; "
        "  public static method Main(): int "
        "    var c := new C() with _v := 7 end; return c._v; "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(codegen_cg34, with_sets_auto_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property P: int; "
        "  public static method Main(): int "
        "    var c := new C() with P := 42 end; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

// fix #747 R43 / §22.11 / §28.3: if a setter (or a setter's value expression)
// throws during a CLASS object-initializer block, the partially-initialized
// instance is disposed by the implicit try/finally before the exception
// propagates. Previously the freshly-`new`'d instance was silently abandoned on
// the throw path (its destructor never ran). All exits are caught, so the JIT
// process never aborts; exit codes are < 256 (no truncation).

// A setter value throws → the instance's destructor runs (W.disposed → 1).
BEZEL_TEST(codegen_cg34, object_init_setter_throws_disposes_instance_747) {
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
        "      using var w := new W() with X := C.Bang() end; "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// A later setter throws after an earlier one stored (§22.11: preceding
// assignments are not rolled back, but the instance IS disposed).
BEZEL_TEST(codegen_cg34, object_init_second_setter_throws_disposes_747) {
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
        "      using var w := new W() with A := 10, B := C.Bang() end; "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// SAFETY (must not double-dispose): a successful object-initializer disposes
// the instance EXACTLY ONCE at scope end — the implicit try/finally guard is
// removed on the normal path, leaving only the `using var` discharge.
BEZEL_TEST(codegen_cg34, object_init_success_disposes_once_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  public X: int := 0; "
        "  public static disposed: int := 0; "
        "  destructor W.disposed := W.disposed + 1; end destructor; "
        "end W; "
        "class C public static method Main(): int "
        "  do using var w := new W() with X := 5 end; end; "
        "  return W.disposed; "  // 1, not 2
        "end Main; end C;"), 1);
}

// SAFETY (must not double-dispose): a successful object-initializer followed by
// a LATER throw while the instance is bound disposes it exactly once (the
// `using var` discharge on unwind; the removed guard does not also fire).
BEZEL_TEST(codegen_cg34, object_init_success_then_later_throw_disposes_once_747) {
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
        "      using var w := new W() with X := 5 end; "
        "      var z := C.Bang(); "
        "    catch Exception => return W.disposed; "  // 1
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 1);
}

// fix #747 R43 / §22.11: a class object initializer "assigns through the named
// properties' setters". A property with a CUSTOM (block) setter must run its
// setter body — previously the with-block only handled fields + auto-properties
// (via backing storage), so a custom-setter target was silently dropped and any
// exception the setter throws was swallowed. All exits are caught.

// The setter body runs and transforms the value (doubles it): with V := 5 → 10.
BEZEL_TEST(codegen_cg34, object_init_runs_custom_setter_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W "
        "  private _v: int := 0; "
        "  public property V: int get => _v; set => do _v := value * 2; end; end; "
        "end W; "
        "class C public static method Main(): int "
        "  using var w := new W() with V := 5 end; "
        "  return w.V; "  // 10 — the setter ran
        "end Main; end C;"), 10);
}

// A setter that throws propagates out of the object initializer (was swallowed).
BEZEL_TEST(codegen_cg34, object_init_setter_throw_propagates_747) {
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
        "    using var w := new W() with V := 5 end; "
        "    return 0; "
        "  catch InvalidOperationException => return 1; end; "  // 1
        "end Main; end C;"), 1);
}

// §22.11 + §28.3: when the setter throws, the instance is also disposed by the
// implicit try/finally (the [13] guard) before the exception propagates.
BEZEL_TEST(codegen_cg34, object_init_setter_throw_disposes_747) {
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
        "    using var w := new W() with V := 5 end; "
        "    return 99; "
        "  catch InvalidOperationException => return W.disposed; end; "  // 1
        "end Main; end C;"), 1);
}

// CONTROL: an auto-property target still works (its setter is a plain field
// store via backing storage; the custom-setter path must not change it).
BEZEL_TEST(codegen_cg34, object_init_auto_property_unregressed_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W public property P: int; end W; "
        "class C public static method Main(): int "
        "  using var w := new W() with P := 42 end; "
        "  return w.P; "  // 42
        "end Main; end C;"), 42);
}

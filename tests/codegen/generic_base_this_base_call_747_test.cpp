// Codegen — `this.M()` / `base.M()` calling an INHERITED method whose return
// type is the generic base's type parameter, with a non-int type argument
// (§12.3 / §14.1 / §22.6, #747 R42).
//
// A `this`/`base` keyword receiver carries no sema exprType, so neither the
// #722 sema base-chain substitution nor the #585 codegen ancestor-substitution
// fired for an inherited generic-return method. The base method then ran as the
// T→i32 template: sema left the return type `?` (FALSE_REJECT with operators),
// and codegen returned i32 where a ptr/struct was expected (IR_FAIL on a string
// return) or read a record at i32 width (silent wrong value). The fix seeds the
// base-chain walk from the enclosing class (currentType_ in sema;
// indexReceiverDecl's reverse-lookup in codegen) for a this/base receiver.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Base<T> with a value field and a method returning T, plus a non-generic
// Derived that fixes T. Closes over the disposal obligation with d.Dispose().
const char* kBaseInt =
    "module M; "
    "class Base<T> "
    "  public V: T; "
    "  public constructor(v: T) V := v; end constructor; "
    "  public method GetVal(): T return V; end GetVal; "
    "end Base; ";
}  // namespace

// FALSE_REJECT (finding 3): `this.GetVal() + 5` on an inherited GetVal(): T,
// T=int. The return type was `?`, so `+` was rejected. Now types as int → 15.
BEZEL_TEST(codegen_generic_base_747, this_call_inherited_int_operator) {
    BEZEL_EXPECT_EQ(run(std::string(kBaseInt) +
        "class Derived: Base<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Tag(): int return this.GetVal() + 5; end Tag; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(10); var r := d.Tag(); d.Dispose(); return r; "
        "end Main; end C;"), 15);
}

// Same via the `base` keyword.
BEZEL_TEST(codegen_generic_base_747, base_call_inherited_int_operator) {
    BEZEL_EXPECT_EQ(run(std::string(kBaseInt) +
        "class Derived: Base<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Tag(): int return base.GetVal() + 5; end Tag; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(10); var r := d.Tag(); d.Dispose(); return r; "
        "end Main; end C;"), 15);
}

// IR_FAIL (finding 1): an override `GetVal(): string return base.GetVal();`
// where the virtual base GetVal returns T=string. The base call lowered as i32
// vs a ptr return → "Function return type does not match operand type". Now the
// string round-trips; "hi".Length == 2.
BEZEL_TEST(codegen_generic_base_747, base_call_inherited_string_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public virtual method GetVal(): T return V; end GetVal; "
        "end Base; "
        "class Derived: Base<string> "
        "  public constructor(v: string) base(v); end constructor; "
        "  public override method GetVal(): string return base.GetVal(); end GetVal; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(\"hi\"); var s := d.GetVal(); var r := s.Length; "
        "  d.Dispose(); return r; "
        "end Main; end C;"), 2);
}

// WRONG_VALUE (finding 2): `this.GetVal()` with T = a record, result used. The
// record was read through the i32 placeholder → silent 0. Now → 40 + 2 = 42.
BEZEL_TEST(codegen_generic_base_747, this_call_inherited_record_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Base<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public method GetVal(): T return V; end GetVal; "
        "end Base; "
        "class Derived: Base<Pt> "
        "  public constructor(v: Pt) base(v); end constructor; "
        "  public method Sum(): int var p := this.GetVal(); return p.x + p.y; end Sum; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(new Pt(40, 2)); var r := d.Sum(); d.Dispose(); return r; "
        "end Main; end C;"), 42);
}

// CONTROL (must NOT regress): generic-SELF `this.Get()` on Box<T> itself (parent
// == enclosing class, so the new base-chain seeding is a no-op). T=int → 42.
BEZEL_TEST(codegen_generic_base_747, generic_self_this_call_unregressed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public method Get(): T return V; end Get; "
        "  public method Echo(): T return this.Get(); end Echo; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(42); var r := b.Echo(); b.Dispose(); return r; "
        "end Main; end C;"), 42);
}

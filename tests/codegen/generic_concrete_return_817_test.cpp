// Codegen — issue #817: a method of a GENERIC class that returns a CONCRETE
// (non-type-parameter) type while reading a generic-typed field emitted
// malformed IR (§14.2 monomorphization).
//
// THE BUG: a generic class's method bodies are emitted twice — (1) a correct
// per-instantiation specialization, used at every call (the receiver always
// binds the concrete type-arg substitution), and (2) an unspecialized TEMPLATE
// emitted only so the module verifies. The template lowers `T`-typed
// fields/locals at the i32 / `{i8,i32}` placeholder layout. When the method's
// SIGNATURE is concrete — e.g. `method GetConcrete(): Counter?` returning a
// `T?` field — the template's placeholder body (`{i8,i32}`) disagreed with the
// concrete return type (`ptr`), so the whole module failed IR verification:
//   "Function return type does not match operand type of return inst!
//    ret { i8, i32 } %2 / ptr".
//
// THE FIX: emitFunctionBody emits a type-safe stub (`ret null-of-return-type`)
// for a generic-class / generic-method TEMPLATE instead of the real, possibly
// ill-typed body. The template is dead code; every call routes to a
// specialization. The third test pins the key soundness property — a
// concrete-return method that ignores `T` still runs its REAL body (via the
// specialization), not the stub.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The verified repro: a concrete nullable class return reading a `T?` field.
BEZEL_TEST(generic_concrete_return_817, nullable_concrete_return_from_t_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method GetConcrete(): Counter? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(7)); "
        "  var c := h.GetConcrete(); "
        "  return c!.id; "  // 7
        "end Main; end C;"), 7);
}

// The same class also returning T? (a true type-parameter return) must keep
// working — the specialization path is unaffected by the template stub.
BEZEL_TEST(generic_concrete_return_817, t_field_round_trip_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method GetT(): T? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(9)); "
        "  var g := h.GetT(); "
        "  return g!.id; "  // 9
        "end Main; end C;"), 9);
}

// Soundness pin: a concrete-int method that ignores T must run its REAL body
// (42 via the specialization), NOT the dead template stub (which would return 0).
BEZEL_TEST(generic_concrete_return_817, concrete_method_ignoring_t_runs_real_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method Tag(): int => 42; "
        "  public method GetT(): T? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(5)); "
        "  return h.Tag() + h.GetT()!.id; "  // 42 + 5 = 47
        "end Main; end C;"), 47);
}

// #902 §12/§14.2: a CONCRETE (non-generic) class derived from a generic base —
// `class CtrHolder : Holder<Counter>` — must bind the base's type-argument
// substitution when an INHERITED generic-base method is called on it. The
// receiver has no generic args of its own, so the inherited-method genericSubst
// branch's `!recvTy->genericArgs.empty()` guard excluded it and the call
// dispatched to the type-erased template (which after #817 nulls its
// irreconcilable T-typed return → wrong null). Now it specializes to
// Holder<Counter>.
BEZEL_TEST(generic_concrete_return_817, concrete_derived_inherited_method_902) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method GetConcrete(): Counter? => _val; "
        "end Holder; "
        "class CtrHolder : Holder<Counter> "
        "  public constructor(owned v: Counter) base(move v); end constructor; "
        "  destructor end destructor; "
        "end CtrHolder; "
        "class C public static method Main(): int "
        "  using var h := new CtrHolder(new Counter(7)); "
        "  var c := h.GetConcrete(); "
        "  return (c!.id if c is not null else -1); "  // 7
        "end Main; end C;"), 7);
}

// #902: a value-typed T inherited method that READS the T-typed field through
// the concrete-derived receiver returns the right value (IntBox : Box<int>).
BEZEL_TEST(generic_concrete_return_817, concrete_derived_value_t_inherited_902) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; "
        "  public constructor(x: T) _v:=x; end constructor; destructor end destructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "class IntBox : Box<int> "
        "  public constructor(x: int) base(x); end constructor; destructor end destructor; "
        "end IntBox; "
        "class C public static method Main(): int "
        "  using var b := new IntBox(42); return b.Get(); "  // 42
        "end Main; end C;"), 42);
}

// #902: a TRANSITIVE concrete base chain (Leaf : Mid : Box<int>) still binds the
// generic base's substitution for the inherited method.
BEZEL_TEST(generic_concrete_return_817, concrete_derived_transitive_base_902) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; "
        "  public constructor(x: T) _v:=x; end constructor; destructor end destructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "class Mid : Box<int> "
        "  public constructor(x: int) base(x); end constructor; destructor end destructor; end Mid; "
        "class Leaf : Mid "
        "  public constructor(x: int) base(x); end constructor; destructor end destructor; end Leaf; "
        "class C public static method Main(): int "
        "  using var b := new Leaf(99); return b.Get(); "  // 99
        "end Main; end C;"), 99);
}

// ── §14 / §12 issue #989: BARE-IDENTIFIER access of an INHERITED GENERIC field ─
// A concrete non-generic subclass of a generic base must substitute the base's
// T-arg for an inherited field accessed via bare identifier (implicit this.field)
// inside a subclass method body.  Without the fix the sema path typed the field
// as `?` (symptom D — false-reject of `val + val`) and the codegen path used
// the i32 placeholder layout (symptom A — IR_FAIL on string return; symptom B/C
// — SIGSEGV / wrong-value for a class-pointer field).

// Symptom A: inherited generic string field returned via bare identifier (`val`)
// — previously IR_FAIL (`ret i32 / ptr`). Expected exit = 5 (length of "hello").
BEZEL_TEST(generic_concrete_return_817, inherited_generic_string_field_bare_id_989) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder<T> public val: T; "
        "  public constructor(v: T) val := v; end constructor; end Holder; "
        "class StrHolder : Holder<string> "
        "  public constructor(v: string) base(v); end constructor; "
        "  public method Get(): string => val; "  // bare 'val' — §14/§12 #989
        "end StrHolder; "
        "class C public static method Main(): int "
        "  using var h := new StrHolder(\"hello\"); return h.Get().Length; "
        "end Main; end C;"), 5);
}

// Symptom B/C: inherited generic class field accessed via bare identifier
// — previously SIGSEGV (i32 loaded as ptr) / wrong-value 0. Expected exit = 42.
BEZEL_TEST(generic_concrete_return_817, inherited_generic_class_field_bare_id_989) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public n: int; "
        "  public constructor(x: int) n := x; end constructor; "
        "  destructor end destructor; end Box; "
        "class Wrapper<T> public owned val: T?; "
        "  public constructor(owned v: T) val := v; end constructor; "
        "  destructor end destructor; end Wrapper; "
        "class BoxWrapper : Wrapper<Box> "
        "  public constructor(owned v: Box) base(move v); end constructor; "
        "  destructor end destructor; "
        "  public method GetN(): int => val!.n; "  // bare 'val' — §14/§12 #989
        "end BoxWrapper; "
        "class C public static method Main(): int "
        "  using var w := new BoxWrapper(new Box(42)); return w.GetN(); "
        "end Main; end C;"), 42);
}

// Symptom D: bare-identifier inherited generic int field used in an expression
// — previously sema typed it as `?`, rejecting `val + val` with
// "operator `+` not defined for type `?`". Expected exit = 42 (21 + 21).
BEZEL_TEST(generic_concrete_return_817, inherited_generic_int_field_operator_989) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public val: T; "
        "  public constructor(v: T) val := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class IntBox : Box<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  destructor end destructor; "
        "  public method Double(): int => val + val; "  // bare 'val' — §14/§12 #989
        "end IntBox; "
        "class C public static method Main(): int "
        "  using var b := new IntBox(21); return b.Double(); "
        "end Main; end C;"), 42);
}

// Dispose-exactly-once: an owned inherited generic field (T=class with destructor)
// is disposed exactly once when the containing CellHolder goes out of scope.
// A counter incremented on construction and decremented on destruction must end
// at 0 after the `using var` scope closes (no leak, no double-free).
BEZEL_TEST(generic_concrete_return_817, inherited_owned_generic_field_dispose_once_989) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class Holder<T> public owned val: T?; "
        "  public constructor(owned v: T) val := v; end constructor; "
        "  destructor end destructor; end Holder; "
        "class CellHolder : Holder<Cell> "
        "  public constructor(owned v: Cell) base(move v); end constructor; "
        "  destructor end destructor; end CellHolder; "
        "class C public static method Main(): int "
        "  do using var h := new CellHolder(new Cell()); end; "
        "  return Cell.n; "  // 0: disposed exactly once
        "end Main; end C;"), 0);
}

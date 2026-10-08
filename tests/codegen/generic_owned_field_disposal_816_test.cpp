// Codegen — issue #816: a generic class's `owned` type-parameter-typed field was
// not disposed when it appeared as a BASE class's field or as an owned generic
// OWNING-collection field, because such a class fell back to the shared,
// type-erased disposer (where the concrete element/field type is unknown). The
// owned payload leaked (§28.5 instantiation-time classification / §28.11).
//
// Each test uses the destructor-counter idiom: `Cell.n` is incremented in the
// constructor and decremented in the destructor; an exit code of 0 means every
// Cell was disposed exactly once (a positive code is a leak, a negative one a
// double-free).
//
// THE FIX:
//  - classHasOwnedTypeParamField now also detects (b) a direct owned generic
//    owning-collection field whose element is a type parameter, and (c) an
//    inherited owned type-parameter field, so the class gets a per-instantiation
//    disposer.
//  - emitOwnedFieldDisposals resolves a type-parameter collection element to its
//    concrete type before elementPolicyFor (so the element disposer thunk is
//    built).
//  - emitBaseDisposeChain calls a SPECIALIZED base core disposer (binding the
//    base's concrete type args from baseTypeRefs) instead of the shared one,
//    covering both `Child<T> : Base<T>` and the non-generic `ConcreteChild :
//    Base<Cell>`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// (a) a generic subclass of a generic base — the base's owned `_field: T` must be
// disposed via a specialized base core.
BEZEL_TEST(generic_owned_field_disposal_816, generic_child_disposes_inherited_owned_t) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Base<T> public owned _field: T; "
        "  public constructor(owned f: T) _field:=f; end constructor; "
        "  destructor end destructor; end Base; "
        "class Child<T> : Base<T> "
        "  public constructor(owned f: T) base(move f); end constructor; "
        "  destructor end destructor; end Child; "
        "class C public static method Main(): int "
        "  do using var c := new Child<Cell>(new Cell()); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// (b) a generic class with an owned OwningArray<T> field — its elements must be
// disposed (the per-instantiation disposer resolves T to the concrete element).
BEZEL_TEST(generic_owned_field_disposal_816, generic_owned_array_field_disposes_elements) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Container<T> private owned _arr: OwningArray<T>; "
        "  public constructor(owned arr: OwningArray<T>) _arr:=arr; end constructor; "
        "  destructor end destructor; end Container; "
        "class C public static method Main(): int "
        "  var arr := new OwningArray<Cell>(); "
        "  arr.Add(new Cell()); arr.Add(new Cell()); "
        "  do using var c := new Container<Cell>(move arr); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// (c) a NON-generic class deriving from a concrete generic instantiation — the
// base's owned `_field: T` (T=Cell via baseTypeRefs) must be disposed.
BEZEL_TEST(generic_owned_field_disposal_816, concrete_child_disposes_inherited_owned_t) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Base<T> public owned _field: T; "
        "  public constructor(owned f: T) _field:=f; end constructor; "
        "  destructor end destructor; end Base; "
        "class ConcreteChild : Base<Cell> "
        "  public constructor(owned f: Cell) base(move f); end constructor; "
        "  destructor end destructor; end ConcreteChild; "
        "class C public static method Main(): int "
        "  do using var c := new ConcreteChild(new Cell()); end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// Control: a value-type instantiation must NOT spuriously dispose / crash — the
// owned int field has no disposal, Cell is untouched (n stays 0).
BEZEL_TEST(generic_owned_field_disposal_816, value_instantiation_no_spurious_disposal) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Base<T> public owned _field: T; "
        "  public constructor(owned f: T) _field:=f; end constructor; "
        "  destructor end destructor; end Base; "
        "class Child<T> : Base<T> "
        "  public constructor(owned f: T) base(move f); end constructor; "
        "  destructor end destructor; end Child; "
        "class C public static method Main(): int "
        "  using var probe := new Cell(); "      // n=1 throughout
        "  do using var v := new Child<int>(5); end; "  // no Cell involved
        "  return Cell.n; "  // 1 (only probe is live)
        "end Main; end C;"), 1);
}

// ===========================================================================
// #937: a generic class with a bare owned type-PARAMETER field (`owned _v: T`)
// instantiated with T = a stdlib OWNING COLLECTION (OwningArray/OwningStack/
// OwningLinkedList/OwningDictionary). The field was classified as a plain owned
// class reference (vtable Dispose freed the backing but did not cascade into
// the elements), so the elements leaked. ownedCollectionFieldType now resolves
// the type param to its concrete instantiation and routes a collection-typed
// param through the element-cascading collection disposal path.
// ===========================================================================

namespace {
const char* kCellBox =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n+1; end constructor; "
    "  destructor Cell.n := Cell.n-1; end destructor; end Cell; "
    "class Box<T> where T: class public owned _v: T; "
    "  public constructor(owned v: T) _v := v; end constructor; end Box; ";
}  // namespace

// T = OwningArray<Cell>: the array's class element is disposed (no leak).
BEZEL_TEST(generic_owned_field_disposal_816, owned_type_param_array_disposes_elements_937) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCellBox +
        "class Program public static method Main(): int "
        "  do "
        "    var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell()); arr.Add(new Cell()); "
        "    using var b := new Box<OwningArray<Cell>>(move arr); "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end Program;"), 0);
}

// T = OwningDictionary<string, Cell>: the owned V values are disposed.
BEZEL_TEST(generic_owned_field_disposal_816, owned_type_param_dict_disposes_values_937) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCellBox +
        "class Program public static method Main(): int "
        "  do "
        "    var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"a\", new Cell()); d.Add(\"b\", new Cell()); "
        "    using var b := new Box<OwningDictionary<string, Cell>>(move d); "
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end Program;"), 0);
}

// T = plain Cell still works (no spurious collection routing / no regression).
BEZEL_TEST(generic_owned_field_disposal_816, owned_type_param_plain_class_still_disposes_937) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCellBox +
        "class Program public static method Main(): int "
        "  do using var b := new Box<Cell>(new Cell()); end; "
        "  return Cell.n; "  // 0
        "end Main; end Program;"), 0);
}

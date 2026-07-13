// Codegen — a generic method/indexer whose return type is a bare type
// parameter T, instantiated so T is a RECORD value (§14.1 generics are
// monomorphized; §9 a record is a value type with by-value semantics).
//
// THE BUG (verified): `class Box<T> ... method GetIt(): T => _v` instantiated
// as `Box<Pair>` (Pair a 2-field record) returned garbage — the call was typed
// as the erased `T`, so the result local was sized to the i32 generic fallback
// and the multi-field record was truncated/zeroed at the use site (both fields
// read 0). The same defect hit a generic *indexer* `Box<Pair>[i]: T`: the
// expression getter body read its `T`-typed field as the i32 erasure rather
// than the substituted record struct.
//
// THE FIX has two parts, both keyed on §14.1's monomorphization:
//   1. Sema (typeCall): a method whose declared return is a bare type parameter
//      of its parent class has the substituted type at a concrete instantiation
//      — `b.GetIt()` on `b: Box<Pair>` is a `Pair`. (substituteMemberType,
//      already used for `T`-typed fields/properties and `T`-typed indexers.)
//   2. Codegen (lowerIndexerGet): install the receiver's generic substitution
//      before inlining the expression getter body, so a `T`-typed field read
//      loads the concrete record struct, not its i32 erasure.
//
// Controls guard the working paths: a SCALAR generic return (Box<int>.GetIt()
// → int), a NON-generic record-returning method, and the value-typed user
// indexer remain correct. A record with a STRING field (a managed reference,
// §3) round-trips its pointer+int pair correctly through a generic method.
//
// All tests use only real stdlib surface; each FAILED before the fix and passes
// after.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── Generic METHOD returning T = record ──────────────────────────────────────

// §14.1 / §9: `Box<Pair>.GetIt()` returns the stored Pair by value; BOTH fields
// (A=10, B=20) must survive. Before the fix the result read 0 (record truncated
// to the i32 generic fallback at the typed-as-`T` use site).
BEZEL_TEST(generic_record_return_14_1, method_returns_two_field_record_both_fields) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetIt(): T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Pair>(new Pair(10, 20)); "
        "  var v := b.GetIt(); "
        "  Console.WriteLine(v.A * 1000 + v.B); return 0; "          // 10020
        "end Main; end C;"), "10020");
}

// §14.1 / §9: a 4-field record (sret-class width) returned from a generic
// method — all four fields correct (1,2,3,4 → 1234).
BEZEL_TEST(generic_record_return_14_1, method_returns_four_field_record_all_fields) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Quad(A: int, B: int, C: int, D: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetIt(): T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Quad>(new Quad(1, 2, 3, 4)); "
        "  var v := b.GetIt(); "
        "  Console.WriteLine(v.A * 1000 + v.B * 100 + v.C * 10 + v.D); return 0; "   // 1234
        "end Main; end C;"), "1234");
}

// §14.1 / §9: the record result used directly in arithmetic (no intermediate
// `var`), confirming the call types as the record (its fields resolve) rather
// than the erased `T`.
BEZEL_TEST(generic_record_return_14_1, method_record_result_fields_in_arithmetic) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetIt(): T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Pair>(new Pair(60, 7)); "
        "  Console.WriteLine(b.GetIt().A * 1000 + b.GetIt().B); return 0; "          // 60007
        "end Main; end C;"), "60007");
}

// §14.1 / §9 / §3: a record with a STRING field (a managed reference) returned
// generically — the pointer+int pair round-trips; the string's Length and the
// int field are both correct (no truncation, no leak/UAF: the string is a
// borrowed literal, not owned).
BEZEL_TEST(generic_record_return_14_1, method_returns_record_with_string_field) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Named(Name: string, Age: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetIt(): T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Named>(new Named(\"hello\", 6)); "
        "  var v := b.GetIt(); "
        "  Console.WriteLine(v.Name.Length * 1000 + v.Age); return 0; "              // 5*1000 + 6 = 5006
        "end Main; end C;"), "5006");
}

// ── Generic INDEXER returning T = record ─────────────────────────────────────

// §14.1 / §23.4 / §9: a generic expression-bodied indexer `[i: int]: T` on
// `Box<Pair>` returns the stored Pair; both fields survive. Before the fix the
// getter body read its `T`-typed field as the i32 erasure (one field, the other
// garbage).
BEZEL_TEST(generic_record_return_14_1, indexer_returns_two_field_record_both_fields) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Pair>(new Pair(10, 20)); "
        "  var v := b[0]; "
        "  Console.WriteLine(v.A * 1000 + v.B); return 0; "          // 10020
        "end Main; end C;"), "10020");
}

// §14.1 / §23.4 / §9: a 4-field record (sret-width) through the generic indexer
// — all fields correct.
BEZEL_TEST(generic_record_return_14_1, indexer_returns_four_field_record_all_fields) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Quad(A: int, B: int, C: int, D: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Quad>(new Quad(1, 2, 3, 4)); "
        "  var v := b[0]; "
        "  Console.WriteLine(v.A * 1000 + v.B * 100 + v.C * 10 + v.D); return 0; "   // 1234
        "end Main; end C;"), "1234");
}

// §14.1 / §23.4 / §9: direct field access on the generic-indexer result
// (`b[0].A`, `b[0].B`) — the record materializes per access; each field correct.
BEZEL_TEST(generic_record_return_14_1, indexer_record_result_direct_field_access) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Pair>(new Pair(60, 7)); "
        "  Console.WriteLine(b[0].A * 1000 + b[0].B); return 0; "    // 60007
        "end Main; end C;"), "60007");
}

// ── Controls — the fix must not perturb these working paths ───────────────────

// CONTROL (§14.1): a SCALAR generic return (Box<int>.GetIt() → int) is
// unaffected — the substitution maps T→int, the existing scalar path applies.
BEZEL_TEST(generic_record_return_14_1, control_scalar_generic_method_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method GetIt(): T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.GetIt(); "                 // 42
        "end Main; end C;"), 42);
}

// CONTROL (§14.1 / §23.4): a SCALAR generic indexer (Box<int>[i] → int) is
// unaffected.
BEZEL_TEST(generic_record_return_14_1, control_scalar_generic_indexer) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b[0]; "                       // 42
        "end Main; end C;"), 42);
}

// CONTROL (§9 / §22): a NON-generic record-returning method still works (the
// path the generic fix models on; its lowering must be unchanged).
BEZEL_TEST(generic_record_return_14_1, control_nongeneric_record_returning_method) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box "
        "  private _v: Pair; "
        "  public constructor(v: Pair) _v := v; end constructor; "
        "  public method GetIt(): Pair => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(new Pair(10, 20)); "
        "  var v := b.GetIt(); "
        "  Console.WriteLine(v.A * 1000 + v.B); return 0; "          // 10020
        "end Main; end C;"), "10020");
}

// CONTROL (§23.4 / §9): a NON-generic record-returning user indexer still works
// (the value-typed-indexer path the generic fix sits beside).
BEZEL_TEST(generic_record_return_14_1, control_nongeneric_record_returning_indexer) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(A: int, B: int); "
        "class Box "
        "  private _v: Pair; "
        "  public constructor(v: Pair) _v := v; end constructor; "
        "  public property [i: int]: Pair => _v; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(new Pair(10, 20)); "
        "  var v := b[0]; "
        "  Console.WriteLine(v.A * 1000 + v.B); return 0; "          // 10020
        "end Main; end C;"), "10020");
}

// #732 / §14 / §22.4: a generic METHOD whose declared return type is a generic
// RECORD (`Make<T>(v: T): Box<T> => new Box<T>(v)`). The method template's
// declared return type lowers `Box<T>` UNspecialized (record.Box), but lowerNew
// installed a T→i32 subst and produced the specialized record.Box$spec0 →
// return-type mismatch / IR-verify failure. lowerNew now skips the substitution
// when every generic arg is an unbound param, matching the template signature.
BEZEL_TEST(generic_record_return, generic_method_returns_generic_record_732) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(Val: T); "
        "class C "
        "  public static method Make<T>(v: T): Box<T> => new Box<T>(v); "
        "  public static method Main(): int "
        "    var b := Make<int>(42); "
        "    return b.Val; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// #732: distinct instantiations (int + string) of the generic-record-returning
// generic method both work.
BEZEL_TEST(generic_record_return, generic_method_returns_generic_record_multi_732) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(Val: T); "
        "class C "
        "  public static method Make<T>(v: T): Box<T> => new Box<T>(v); "
        "  public static method Main(): int "
        "    var b := Make<int>(7); "
        "    var s := Make<string>(\"hi\"); "
        "    return b.Val + s.Val.Length; "  // 7 + 2 = 9
        "  end Main; "
        "end C;"), 9);
}

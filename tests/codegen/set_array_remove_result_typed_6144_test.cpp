// Codegen — the `bool` result of `Remove(item: T)` keeps its type on Set<T>,
// OwningSet<T>, Array<T> and OwningArray<T> (#6144).
//
// QUOTED stdlib signatures (docs/Bezel_Standard_Library.md, v0.338):
//   §4.1  `public interface ICollection<T>`  ... `method Remove(item: T): bool;`
//         (Set<T> is `ICollection<T>`, §4.10; OwningSet<T> inherits the §4.2
//         `IOwningCollection<T>` form `method Remove(item: T): bool;`)
//   §4.4  `Array<T>`:       `public method Remove(item: T): bool;`
//   §4.5  `OwningArray<T>`: `public method Remove(item: T): bool;`
//
// Pre-fix, sema never set the call's result type for these four receivers, so
// `var removed := s.Remove(x)` left `removed` untyped and
// `Console.WriteLine(removed.ToString())` failed to lower (#3079): the whole
// statement emitted nothing. Each receiver is exercised three ways (captured
// in a var, discarded, used as a condition); the oracles observe stdout
// (pre-fix: empty for the var and direct-call forms). A user type's own
// `Remove` must NOT be captured by the new arm.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// Owning rows (OwningSet/OwningArray) run under runAotChecked (always-ASan) and
// loop 100x inside the program (a one-shot leak repro is a vacuous oracle).
// The query is a LOCAL the test owns: `using var q := new Cell(1);` (spec §28.10:
// "`using var` declares a local that is disposed when the enclosing block ends";
// Appendix A5: a scope pin). `Remove` borrows its query: stdlib §4.5 L1759
// "`Remove(item: T): bool;  // borrow-taking query; disposes the matched
// element`" and §4.2 L1602 "`Remove(item: T): bool;  // disposes the removed
// element`" (OwningSet via IOwningCollection<T>). An UNBOUND temporary
// `s.Remove(new Cell(1))` is the separate leaky shape #6177 and is not used here.
static std::string checkedOut(std::string_view src) {
    auto r = bezel::test::runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    std::string o = r.stdoutText;
    if (!o.empty() && o.back() == '\n') o.pop_back();
    return o;
}

BEZEL_TEST(set_array_remove_result_typed_6144, set_var_tostring) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Set<int>(); "
        "  s.Add(1); "
        "  var removed := s.Remove(1); Console.WriteLine(removed.ToString()); var again := s.Remove(1); Console.WriteLine(again.ToString()); "
        "end Main; end C;"), "True\nFalse");
}

BEZEL_TEST(set_array_remove_result_typed_6144, set_discarded) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Set<int>(); "
        "  s.Add(1); "
        "  s.Remove(1); Console.WriteLine(s.Count.ToString()); "
        "end Main; end C;"), "0");
}

BEZEL_TEST(set_array_remove_result_typed_6144, set_condition) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Set<int>(); "
        "  s.Add(1); "
        "  if s.Remove(1) then Console.WriteLine(\"yes\"); end if; if not s.Remove(1) then Console.WriteLine(\"no\"); end if; "
        "end Main; end C;"), "yes\nno");
}

BEZEL_TEST(set_array_remove_result_typed_6144, array_var_tostring) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Array<int>(); "
        "  s.Add(1); "
        "  var removed := s.Remove(1); Console.WriteLine(removed.ToString()); var again := s.Remove(1); Console.WriteLine(again.ToString()); "
        "end Main; end C;"), "True\nFalse");
}

BEZEL_TEST(set_array_remove_result_typed_6144, array_discarded) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Array<int>(); "
        "  s.Add(1); "
        "  s.Remove(1); Console.WriteLine(s.Count.ToString()); "
        "end Main; end C;"), "0");
}

BEZEL_TEST(set_array_remove_result_typed_6144, array_condition) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Array<int>(); "
        "  s.Add(1); "
        "  if s.Remove(1) then Console.WriteLine(\"yes\"); end if; if not s.Remove(1) then Console.WriteLine(\"no\"); end if; "
        "end Main; end C;"), "yes\nno");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningset_var_tostring) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningSet<Cell>(); "
        "  var i := 0; var t := 0; var f := 0; var l1 := \"\"; var l2 := \"\"; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); var removed := s.Remove(q); var again := s.Remove(q); l1 := removed.ToString(); l2 := again.ToString(); if l1 = \"True\" then t := t + 1; end if; if l2 = \"False\" then f := f + 1; end if; i := i + 1; end while; Console.WriteLine(l1); Console.WriteLine(l2); Console.WriteLine(t.ToString()); Console.WriteLine(f.ToString()); "
        "end Main; end C;"), "True\nFalse\n100\n100");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningset_discarded) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningSet<Cell>(); "
        "  var i := 0; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); s.Remove(q); i := i + 1; end while; Console.WriteLine(s.Count.ToString()); "
        "end Main; end C;"), "0");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningset_condition) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningSet<Cell>(); "
        "  var i := 0; var y := 0; var n := 0; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); if s.Remove(q) then y := y + 1; end if; if not s.Remove(q) then n := n + 1; end if; i := i + 1; end while; Console.WriteLine(y.ToString()); Console.WriteLine(n.ToString()); "
        "end Main; end C;"), "100\n100");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningarray_var_tostring) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningArray<Cell>(); "
        "  var i := 0; var t := 0; var f := 0; var l1 := \"\"; var l2 := \"\"; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); var removed := s.Remove(q); var again := s.Remove(q); l1 := removed.ToString(); l2 := again.ToString(); if l1 = \"True\" then t := t + 1; end if; if l2 = \"False\" then f := f + 1; end if; i := i + 1; end while; Console.WriteLine(l1); Console.WriteLine(l2); Console.WriteLine(t.ToString()); Console.WriteLine(f.ToString()); "
        "end Main; end C;"), "True\nFalse\n100\n100");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningarray_discarded) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningArray<Cell>(); "
        "  var i := 0; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); s.Remove(q); i := i + 1; end while; Console.WriteLine(s.Count.ToString()); "
        "end Main; end C;"), "0");
}

BEZEL_TEST(set_array_remove_result_typed_6144, owningarray_condition) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; public override method Equals(other: Object?): bool if other is Cell then var b := other as Cell; return b!.V = V; end; return false; end Equals; public override method GetHashCode(): int => V; end Cell; "
        "class C public static method Main() "
        "  using var s := new OwningArray<Cell>(); "
        "  var i := 0; var y := 0; var n := 0; while i < 100 do s.Add(new Cell(1)); using var q := new Cell(1); if s.Remove(q) then y := y + 1; end if; if not s.Remove(q) then n := n + 1; end if; i := i + 1; end while; Console.WriteLine(y.ToString()); Console.WriteLine(n.ToString()); "
        "end Main; end C;"), "100\n100");
}

// Control: a USER type's own `Remove` keeps its declared result type; the new
// arm is gated on the four builtin collection heads only.
BEZEL_TEST(set_array_remove_result_typed_6144, user_type_remove_is_not_captured) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Bag public method Remove(x: int): string => \"bag\" + x.ToString(); end Bag; "
        "record Rec(A: int) public method Remove(x: int): bool => x = A; end Rec; "
        "class C public static method Main() "
        "  var b := new Bag(); var r := b.Remove(3); Console.WriteLine(r); "
        "  var rc := new Rec(4); var r2 := rc.Remove(4); Console.WriteLine(r2.ToString()); "
        "end Main; end C;"), "bag3\nTrue");
}

// Control: the families that already typed `Remove` are unchanged.
BEZEL_TEST(set_array_remove_result_typed_6144, linkedlist_dictionary_sortedset_remove_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var l := new LinkedList<int>(); l.AddLast(1); "
        "  using var d := new Dictionary<int, int>(); d.Add(1, 1); "
        "  using var q := new SortedSet<int>(); q.Add(1); "
        "  var a := l.Remove(1); var b := d.Remove(1); var c := q.Remove(1); "
        "  Console.WriteLine(a.ToString()); Console.WriteLine(b.ToString()); Console.WriteLine(c.ToString()); "
        "end Main; end C;"), "True\nTrue\nTrue");
}

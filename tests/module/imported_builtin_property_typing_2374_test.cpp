// Cross-module rows for #2374 (the imported-builtin head-name sites): a
// property read on an IMPORTED builtin collection / StringBuilder must be typed
// exactly as on a local one (spec §2.5, v0.340: an imported declaration has the
// same meaning in the importer as in its home module).
//
// ── THE BUG ────────────────────────────────────────────────────────────────
//
// ModuleLoader rebuilds an imported builtin TypeRef as Named +
// `declaration == nullptr` + `astNode == nullptr`, with the head only in
// `importedName`. Three property-typing arms in sema's `resolveMemberAccess`
// (the int `Length` / `Count` properties, `IsReadOnly`, `SortedSet`'s
// `First` / `Last`, and the `LinkedList` / `LinkedListNode` properties) were gated on `tt->astNode`, so an imported receiver was
// left UNTYPED. The visible effects, MEASURED at c0a1928b:
//   * `b.Length.ToString()` on an `OwningBuffer<int>` did not compile;
//   * `var n := sb.Length` (StringBuilder), `var n := s.Count` (SortedSet),
//     `var r := a.IsReadOnly` and `var f := s.First` gave untyped locals
//     whose `ToString()` printed nothing;
//   * `a.Length` on an `Array<int>` — which declares only `Count` — compiled
//     and printed the count instead of being rejected (a silent acceptance);
//   * `l.FirstNode` / `f.Next` / `f.Previous` on an imported `LinkedList`
//     were untyped and their lines printed nothing, `l.First(pred)` resolved
//     to the §5 LINQ operator instead of being rejected (§26.5 rule 1), and
//     `l.First.Value` (a member read on the `int?` element) compiled.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules`: the two-module AOT path, the only one that produces the
// imported TypeRef. Every accept row asserts the exact stdout; the reject row
// asserts the stdlib §4.4 diagnostic the local program gets.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib2374 =
    "module Lib2374; "
    "public class M2374 "
    "  public static method OB(): owned OwningBuffer<int> "
    "    var b := new OwningBuffer<int>(4); b[0] := 3; return b; "
    "  end OB; "
    "  public static method Arr(): owned Array<int> "
    "    var a := new Array<int>(); a.Add(2); a.Add(8); return a; "
    "  end Arr; "
    "  public static method SS(): owned SortedSet<int> "
    "    var s := new SortedSet<int>(); _ := s.Add(7); _ := s.Add(3); return s; "
    "  end SS; "
    "  public static method Sb(): owned StringBuilder "
    "    var b := new StringBuilder(); _ := b.Append(\"abc\"); return b; "
    "  end Sb; "
    "  public static method LL(): owned LinkedList<int> "
    "    var l := new LinkedList<int>(); l.AddLast(4); l.AddLast(9); return l; "
    "  end LL; "
    "  public static method LS(): owned LinkedList<string> "
    "    var l := new LinkedList<string>(); l.AddLast(\"a\" + \"b\"); "
    "    l.AddLast(\"c\" + \"d\"); return l; "
    "  end LS; "
    "end M2374;";

std::string app2374(const std::string& body) {
    return "module App2374; "
           "import Lib2374; "
           "class C2374 public static method Main(): int "
           "  " + body + " "
           "  return 0; "
           "end Main; end C2374;";
}

test::JitResult run2374(const std::string& body) {
    return test::runAotModules(
        {{"Lib2374", kLib2374}, {"App2374", app2374(body)}}, "App2374");
}

}  // namespace

// stdlib §4.14: `OwningBuffer<T>.Length: int`. At c0a1928b the `ToString()`
// call "produced no value" and the program did not compile.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_owning_buffer_length_to_string_spec_4_14) {
    auto r = run2374(
        "using var b := M2374.OB(); Console.WriteLine(b.Length.ToString()); "
        "var n := b.Length; Console.WriteLine(n + 1);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n5\n"));
}

// stdlib §3.1: `StringBuilder.Length: int`. At c0a1928b the inferred local was
// untyped and nothing printed.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_string_builder_length_into_local_spec_3_1) {
    auto r = run2374(
        "using var b := M2374.Sb(); var n := b.Length; "
        "Console.WriteLine(n.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// stdlib §4.18: `SortedSet<T>.Count: int`. At c0a1928b nothing printed.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_sorted_set_count_into_local_spec_4_18) {
    auto r = run2374(
        "using var s := M2374.SS(); var n := s.Count; "
        "Console.WriteLine(n.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// stdlib §4.18: `SortedSet<T>.First` is the least element. At c0a1928b the
// inferred local was untyped and its line printed nothing (only `s.Last`'s 7).
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_sorted_set_first_into_local_spec_4_18) {
    auto r = run2374(
        "using var s := M2374.SS(); var f := s.First; "
        "Console.WriteLine(f.ToString()); Console.WriteLine(s.Last);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n7\n"));
}

// stdlib §4.1: `IsReadOnly: bool` is false on `Array<T>`. At c0a1928b nothing
// printed.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_array_is_read_only_into_local_spec_4_1) {
    auto r = run2374(
        "using var a := M2374.Arr(); var r := a.IsReadOnly; "
        "Console.WriteLine(r.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// stdlib §4.4: `Array<T>` declares `Count`, not `Length`, so `a.Length` is
// rejected exactly as it is on a local `Array<int>`. At c0a1928b it compiled
// and printed 2.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_array_length_rejected_spec_4_4) {
    auto r = run2374(
        "using var a := M2374.Arr(); Console.WriteLine(a.Length);");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(test::diagnosticMessageContains(
        r, "no member `Length` on `Array<T>`: use `Count`"));
}

// stdlib §4.6: `FirstNode: LinkedListNode<T>?`, `Value: T`,
// `Next: LinkedListNode<T>?`. At c0a1928b neither line printed.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_linked_list_first_node_next_spec_4_6) {
    auto r = run2374(
        "using var l := M2374.LL(); var f := l.FirstNode; "
        "if f <> null then Console.WriteLine(f.Value.ToString()); "
        "var nx := f.Next; if nx <> null then Console.WriteLine(nx.Value + 1); end; end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n10\n"));
}

// stdlib §4.6: `LastNode` / `Previous` over a `string` element. At c0a1928b
// nothing printed.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_linked_list_last_node_previous_string_spec_4_6) {
    auto r = run2374(
        "using var l := M2374.LS(); var f := l.LastNode; "
        "if f <> null then var p := f.Previous; "
        "if p <> null then Console.WriteLine(p.Value); end; end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ab\n"));
}

// spec §26.5 rule 1 + stdlib §4.6: `LinkedList<T>.First` is a 0-arg instance
// property, so `l.First(pred)` cannot reach the same-named §5 LINQ operator;
// it is rejected exactly as on a local list. At c0a1928b it compiled and
// printed 9.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_linked_list_first_with_argument_rejected_spec_26_5) {
    auto r = run2374(
        "using var l := M2374.LL(); "
        "Console.WriteLine(l.First(lambda(x: int) => x > 5));");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(test::diagnosticMessageContains(
        r, "is an instance member of the receiver's declared type"));
}

// stdlib §4.6: `LinkedList<T>.First` is the element `T?`, not a node, so a
// `.Value` read on it (an `int`) has no member to resolve, exactly as on a
// local list. At c0a1928b it compiled and printed nothing.
BEZEL_TEST(imported_builtin_property_typing_2374,
           imported_linked_list_first_is_element_not_node_rejected_spec_4_6) {
    auto r = run2374(
        "using var l := M2374.LL(); var f := l.First; "
        "if f <> null then Console.WriteLine(f.Value); end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(test::diagnosticMessageContains(
        r, "no member `Value` on primitive type `int`"));
}

}  // namespace bezel::module

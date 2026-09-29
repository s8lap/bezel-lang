// Codegen — narrow #4858 (§12.7 / §17.4 / §8.1, v0.307; stdlib §9.6):
// `is` / `is not` / `as` on a STRING operand, and on an Object/interface
// operand that holds a string.
//
// `string` is a sealed reference type. stdlib §9.6 declares
//   `public class String: IEnumerable<char>, IComparable<string>,
//    IEquatable<string>`
// and §8.1 makes `Object` the universal root, which `string` reaches "via [its]
// reference-type heap representation". So a runtime string is an instance of
// exactly `string`, `Object`, `IEnumerable<char>`, `IComparable<string>` and
// `IEquatable<string>` (§12.7: "`is T` tests whether a value is an instance of
// `T`"). It is an instance of nothing else. A null `string?` is an instance of
// no type. `as` yields the operand on a match and `null` otherwise (§17.4).
//
// Pre-fix, measured on r20-c25 eed44f6b:
//   * `s is IShape`, `s is not IShape`, `s is Dog`, `s is int`,
//     `s is IEquatable<string>`, and `s as IShape` all SIGSEGV'd (rc 139).
//     lowerTypeTest's #1071 string guard (`mightHoldString`) covered only
//     Object/interface operands, so a `string`-typed operand ran emitIsA
//     over the bare `bz_string*` and walked its `char* data` as a
//     descriptor chain.
//   * `e is IEquatable<string>` with `e: IEnumerable<char>` holding a string
//     SIGSEGV'd the same way. The declaration-less builtin interface was not
//     in the guard either.
//
// NOT decided here: an Object/interface operand that HOLDS a string, tested
// against a stdlib §9.6 conformance (`(o: Object := s) is IEquatable<string>`).
// It still reads False. The #1071 guard identifies strings through
// bz_obj_is_string, which is a whitelist negation: every vtable-less object (an
// Array, a lazy pipeline, a StringBuilder, an OwningArray, a lambda) reads as
// a string. Answering True there would admit those objects too, and `as` would
// then dispatch string Equals/CompareTo on them (type confusion). That cell
// waits on a positive string identification (#4885). The rows at the bottom
// pin that those vtable-less objects stay False/null.
//
// Value-typed operands (`i is Object`, `p is IShape`, …) are deliberately NOT
// covered here. They await the ruling in #4866. The generic-carrier twin is
// #4849 / #4812.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Shared declarations. IShape is a user interface; Pt is a record that
// implements it; Dog is a class. None of them is related to `string`.
std::string prog(const std::string& body) {
    return
        "module Conf;\n"
        "public interface IShape\n"
        "  method Area(): int;\n"
        "end IShape;\n"
        "public record Pt: IShape\n"
        "  public X: int;\n"
        "  public Y: int;\n"
        "  public method Area(): int\n"
        "    return X * Y;\n"
        "  end Area;\n"
        "end Pt;\n"
        "public class Dog\n"
        "  public constructor() end constructor;\n"
        "end Dog;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var s: string := \"ab\";\n" +
        body +
        "  end Main;\n"
        "end Program;\n";
}

void expectOut(const std::string& src, const char* want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// §12.7 / stdlib §9.6: every target kind, `is` in EXPRESSION position.
// Pre-fix: SIGSEGV at the first unrelated-reference target (`s is IShape`).
BEZEL_TEST(concrete_is_as_decision_4858,
           string_operand_is_expression_position_spec_12_7_stdlib_9_6) {
    expectOut(prog(
        "    Console.WriteLine(s is IShape);\n"
        "    Console.WriteLine(s is Dog);\n"
        "    Console.WriteLine(s is int);\n"
        "    Console.WriteLine(s is Pt);\n"
        "    Console.WriteLine(s is IEquatable<string>);\n"
        "    Console.WriteLine(s is IComparable<string>);\n"
        "    Console.WriteLine(s is IEnumerable<char>);\n"
        "    Console.WriteLine(s is IEquatable<int>);\n"
        "    Console.WriteLine(s is IEnumerable<int>);\n"
        "    Console.WriteLine(s is Object);\n"
        "    Console.WriteLine(s is string);\n"),
        "False\nFalse\nFalse\nFalse\nTrue\nTrue\nTrue\nFalse\nFalse\n"
        "True\nTrue\n");
}

// §12.7: the same cells in `if` position. Here a dropped test used to read
// as silent false, and a descriptor walk used to SIGSEGV.
BEZEL_TEST(concrete_is_as_decision_4858,
           string_operand_is_if_position_spec_12_7_stdlib_9_6) {
    expectOut(prog(
        "    var code := 0;\n"
        "    if s is IShape then code := code + 1; end;\n"
        "    if s is Dog then code := code + 2; end;\n"
        "    if s is int then code := code + 4; end;\n"
        "    if s is IEquatable<string> then code := code + 8; end;\n"
        "    if s is IComparable<string> then code := code + 16; end;\n"
        "    if s is IEnumerable<char> then code := code + 32; end;\n"
        "    if s is Object then code := code + 64; end;\n"
        "    Console.WriteLine(code);\n"),
        "120\n");
}

// §12.7 / §17.4: `is not` is the negation, in both positions.
BEZEL_TEST(concrete_is_as_decision_4858,
           string_operand_is_not_spec_12_7_17_4) {
    expectOut(prog(
        "    Console.WriteLine(s is not IShape);\n"
        "    Console.WriteLine(s is not Dog);\n"
        "    Console.WriteLine(s is not IEquatable<string>);\n"
        "    if s is not IShape then Console.WriteLine(\"notshape\"); "
        "else Console.WriteLine(\"shape\"); end;\n"
        "    if s is not IComparable<string> then Console.WriteLine(\"nocmp\"); "
        "else Console.WriteLine(\"cmp\"); end;\n"),
        "True\nTrue\nFalse\nnotshape\ncmp\n");
}

// §17.4: `as` returns the operand on a match (usable through the interface)
// and `null` otherwise, including a value target (`int?` null).
// Pre-fix: `s as IShape` SIGSEGV'd.
BEZEL_TEST(concrete_is_as_decision_4858, string_operand_as_spec_17_4) {
    expectOut(prog(
        "    Console.WriteLine((s as IShape) = null);\n"
        "    Console.WriteLine((s as Dog) = null);\n"
        "    Console.WriteLine((s as int) = null);\n"
        "    var eq := s as IEquatable<string>;\n"
        "    if eq <> null then Console.WriteLine(eq.Equals(\"ab\")); "
        "else Console.WriteLine(\"null\"); end;\n"
        "    var c := s as IComparable<string>;\n"
        "    if c <> null then Console.WriteLine(c.CompareTo(\"ab\")); "
        "else Console.WriteLine(\"null\"); end;\n"),
        "True\nTrue\nTrue\nTrue\n0\n");
}

// §12.7 / §7: a nullable `string?` operand is an instance only when non-null.
BEZEL_TEST(concrete_is_as_decision_4858,
           nullable_string_operand_spec_12_7_7) {
    expectOut(prog(
        "    var ns: string? := s;\n"
        "    var nn: string? := null;\n"
        "    Console.WriteLine(ns is IEquatable<string>);\n"
        "    Console.WriteLine(nn is IEquatable<string>);\n"
        "    Console.WriteLine(ns is IShape);\n"
        "    Console.WriteLine(nn is not IShape);\n"
        "    Console.WriteLine((ns as IComparable<string>) <> null);\n"
        "    Console.WriteLine((nn as IComparable<string>) = null);\n"),
        "True\nFalse\nFalse\nTrue\nTrue\nTrue\n");
}

// §12.7 / §8.6: an interface carrier that holds a string, tested against an
// UNRELATED target. `IEnumerable<char>` is the declaration-less builtin stdlib
// §9.6 gives `string`. Pre-fix it was not in the #1071 guard, so emitIsA
// walked the string's `char* data` (SIGSEGV). An Object carrier was already
// guarded and is pinned alongside.
BEZEL_TEST(concrete_is_as_decision_4858,
           ienumerable_char_carrier_holding_string_spec_12_7_8_6) {
    expectOut(prog(
        "    var e: IEnumerable<char> := s;\n"
        "    var o: Object := s;\n"
        "    Console.WriteLine(e is IShape);\n"
        "    Console.WriteLine(e is not IShape);\n"
        "    Console.WriteLine((e as IShape) = null);\n"
        "    if e is Dog then Console.WriteLine(\"dog\"); "
        "else Console.WriteLine(\"notdog\"); end;\n"
        "    Console.WriteLine(o is IShape);\n"
        "    Console.WriteLine((o as Dog) = null);\n"),
        "False\nTrue\nTrue\nnotdog\nFalse\nTrue\n");
}

// Loop-amplified (50 iterations): every formerly-SIGSEGV cell, evaluated
// repeatedly over fresh strings, must neither fault nor change its answer.
// The IEnumerable<char> carrier is bound once, outside the loop. Binding a
// non-literal string to it inside a loop leaks independently of any type
// test (#4886).
BEZEL_TEST(concrete_is_as_decision_4858,
           formerly_sigsegv_cells_loop_50_spec_12_7) {
    expectOut(prog(
        "    var e: IEnumerable<char> := s;\n"
        "    var hits := 0;\n"
        "    var misses := 0;\n"
        "    for var k in 0..<50 do\n"
        "      var t: string := s + k.ToString();\n"
        "      if t is IShape then hits := hits + 1; else misses := misses + 1; end;\n"
        "      if t is not IShape then misses := misses + 1; end;\n"
        "      if t is Dog then hits := hits + 1; end;\n"
        "      if t is int then hits := hits + 1; end;\n"
        "      if t is IEquatable<string> then hits := hits + 1; end;\n"
        "      if (t as IShape) = null then misses := misses + 1; end;\n"
        "      if e is IShape then hits := hits + 1; else misses := misses + 1; end;\n"
        "    end;\n"
        "    Console.WriteLine(hits);\n"
        "    Console.WriteLine(misses);\n"),
        "50\n200\n");
}

// Review of the first 8d01023c attempt (§12.7 / §17.4 / §8.1, v0.307): a
// vtable-less NON-string object held in `Object` is not an instance of
// `IEquatable<string>` / `IComparable<string>`. bz_obj_is_string reads every
// one of these as a string (#4885). So a guard whose string arm answered the
// stdlib §9.6 conformances made them True, and `as` bound them and dispatched
// string Equals/CompareTo on an Array. Each carrier must stay False for
// `is`, True for `is not`, and null for `as`.
namespace {
std::string notStringConformers(const std::string& bind) {
    return prog(
        bind +
        "    Console.WriteLine(o is IEquatable<string>);\n"
        "    Console.WriteLine(o is IComparable<string>);\n"
        "    Console.WriteLine(o is not IEquatable<string>);\n"
        "    Console.WriteLine((o as IEquatable<string>) = null);\n"
        "    Console.WriteLine((o as IComparable<string>) = null);\n"
        "    var eq := o as IEquatable<string>;\n"
        "    if eq <> null then Console.WriteLine(eq.Equals(\"x\")); "
        "else Console.WriteLine(\"null\"); end;\n");
}
const char* kNotConformer = "False\nFalse\nTrue\nTrue\nTrue\nnull\n";
}  // namespace

BEZEL_TEST(concrete_is_as_decision_4858,
           array_in_object_not_string_conformer_spec_12_7_17_4) {
    expectOut(notStringConformers(
        "    var a: Array<int> := [1, 2, 3];\n"
        "    var o: Object := a;\n"), kNotConformer);
}

BEZEL_TEST(concrete_is_as_decision_4858,
           pipeline_in_object_not_string_conformer_spec_12_7_17_4) {
    expectOut(notStringConformers(
        "    var a: Array<int> := [1, 2, 3];\n"
        "    var q: IEnumerable<int> := a.Select(lambda(x) => x * 2);\n"
        "    var o: Object := q;\n"), kNotConformer);
}

BEZEL_TEST(concrete_is_as_decision_4858,
           stringbuilder_in_object_not_string_conformer_spec_12_7_17_4) {
    expectOut(notStringConformers(
        "    var sb := new StringBuilder();\n"
        "    sb.Append(\"hi\");\n"
        "    var o: Object := sb;\n"), kNotConformer);
}

BEZEL_TEST(concrete_is_as_decision_4858,
           owningarray_in_object_not_string_conformer_spec_12_7_17_4) {
    expectOut(notStringConformers(
        "    var oa := new OwningArray<Dog>();\n"
        "    oa.Add(new Dog());\n"
        "    var o: Object := oa;\n"), kNotConformer);
}

BEZEL_TEST(concrete_is_as_decision_4858,
           lambda_in_object_not_string_conformer_spec_12_7_17_4) {
    expectOut(notStringConformers(
        "    var f: (int) -> int := lambda(x) => x + 1;\n"
        "    var o: Object := f;\n"), kNotConformer);
}

// §12.7 / stdlib §9.6: an `Array<char>` held in `IEnumerable<char>` is not a
// string, so it conforms to neither `IEquatable<string>` nor
// `IComparable<string>`. Pre-fix the IEnumerable<char> carrier was unguarded:
// emitIsA walked the array (True, and an ASan global-buffer-overflow).
BEZEL_TEST(concrete_is_as_decision_4858,
           char_array_in_ienumerable_char_not_string_conformer_spec_12_7) {
    expectOut(prog(
        "    var a: Array<char> := ['a', 'b'];\n"
        "    var e: IEnumerable<char> := a;\n"
        "    Console.WriteLine(e is IEquatable<string>);\n"
        "    Console.WriteLine(e is IComparable<string>);\n"
        "    Console.WriteLine((e as IEquatable<string>) = null);\n"),
        "False\nFalse\nTrue\n");
}

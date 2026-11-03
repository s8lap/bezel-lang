// Regression tests for #7824 -- a user extension whose name is one of the
// universal `Object` instance members still bound a call on a USER class or
// record receiver, so the extension's answer displaced the committed instance
// member's.
//
// THE SPEC.  Language spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or
// any base type) *declares* any instance member named `Foo` -- of any arity,
// applicability, or accessibility -- instance resolution is committed and
// extension blocks are **never consulted** for that name on that receiver. If
// the committed instance lookup then fails (no applicable overload,
// §22.5/§22.6, or the member is inaccessible, §13.1), the call is a compile
// error citing the instance member(s); it does not fall back to an extension."
//
// §8.1 (v0.350) declares `Object`'s public surface:
//
//     public class Object
//       public method GetType(): Type;
//       public virtual method Equals(other: Object?): bool;
//       public virtual method GetHashCode(): int;
//       public virtual method ToString(): string;
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
// and: "`Object` is the universal root. Every type derives from it: classes
// (including heap-array classes `Array<T>` and `OwningArray<T>`) directly
// through the inheritance chain".  A user class and a user record therefore
// INHERIT those four instance members, and rule 1 commits their NAMES on such
// a receiver -- whatever the arity of the extension that carries the name.
//
// THE DEFECT.  The commitment is INVISIBLE to the ordinary member walk:
// `Object`'s members are not symbols on the user type, so the walk found no
// `ToString` and the resolution fell through to the extension blocks.
// MEASURED: an `extension R { ToString(n: int): string }` bound `x.ToString(3)`
// on a `record R`; an `extension C { Equals(a: int, b: int): bool }` bound
// `x.Equals(1, 2)` on a `class C`; and the bound call evaluated the receiver
// TWICE (the extension's receiver and the `Object` slot's receiver were two
// separate evaluations).
//
// THE REFUSAL IS THE HOUSE ONE.  The committed instance member has no
// MemberSymbol (§8.1's universal members are synthesized), so a call with the
// wrong arity reaches typeCall's universal arity check and is refused with
// "the universal `Object.ToString` member takes 0 argument(s) but the call
// supplies 1 (§8.1)" -- the same refusal the built-in receivers get (#7775's
// `object_base_members_are_not_shadowed_on_an_array` row).
//
// ORACLE: the rejection TEXT for the refused rows, and the observed VALUE for
// the rows where the committed member is the answer.  No row asserts the
// absence of a diagnostic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectOut(const std::string& src, const std::string& out) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A program over a user CLASS `C` (with a `Box` variant below).
std::string classProgram(const std::string& decls, const std::string& body) {
    return "module R;\n" + decls +
           "public class C public constructor(X: int) end constructor; end C;\n"
           "class Program public static method Main(): int\n" + body +
           "  return 0;\n"
           "end Main; end Program;\n";
}

std::string recordProgram(const std::string& decls, const std::string& body) {
    return "module R;\n" + decls +
           "public record Rec(X: int);\n"
           "class Program public static method Main(): int\n" + body +
           "  return 0;\n"
           "end Main; end Program;\n";
}

}  // namespace

// ---- the defect rows --------------------------------------------------------

// §26.5 rule 1 + §8.1: `Object` declares `ToString()`, so a same-named
// extension on a user class is unreachable -- and the call that supplies an
// argument is rule 1's "the committed instance lookup then fails" compile
// error, not the extension's value.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           class_tostring_extension_does_not_bind) {
    expectRejected(
        classProgram("extension C public method ToString(n: int): string => "
                     "\"EXT\"; end;\n",
                     "  var x: C := new C(X := 5);\n"
                     "  Console.WriteLine(x.ToString(3));\n"),
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// §26.5 rule 1 + §8.1: `Object` declares `Equals(other: Object?)`, so a
// two-argument extension of that name is unreachable.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           class_equals_two_arg_extension_does_not_bind) {
    expectRejected(
        classProgram("extension C public method Equals(a: int, b: int): bool "
                     "=> true; end;\n",
                     "  var x: C := new C(X := 5);\n"
                     "  Console.WriteLine(x.Equals(1, 2));\n"),
        "the universal `Object.Equals` member takes 1 argument(s) but the call "
        "supplies 2 (§8.1)");
}

// The same commitment on a user RECORD receiver (`Object` is its base by
// §8.1, and its own `ToString` fills the slot, §9.2).
BEZEL_TEST(object_slot_rule1_user_type_7824,
           record_tostring_extension_does_not_bind) {
    expectRejected(
        recordProgram("extension Rec public method ToString(n: int): string "
                      "=> \"EXT\"; end;\n",
                      "  var x: Rec := new Rec(X := 5);\n"
                      "  Console.WriteLine(x.ToString(3));\n"),
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The same commitment on a user INTERFACE receiver: §8.1 has an interface
// "reach[] `Object` ... via [its] reference-type heap representation", so the
// universal members are present on it (MEASURED: `x.ToString()` on an
// `IFoo` receiver answers) and rule 1 commits their NAMES there too.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           interface_tostring_extension_does_not_bind) {
    expectRejected(
        "module R;\n"
        "public interface IFoo\n"
        "  method Bar(): int;\n"
        "end IFoo;\n"
        "public class C: IFoo public method Bar(): int => 1; end C;\n"
        "extension IFoo public method ToString(n: int): string => \"EXT\"; "
        "end;\n"
        "class Program public static method Main(): int\n"
        "  var x: IFoo := new C();\n"
        "  Console.WriteLine(x.ToString(3));\n"
        "  return 0;\n"
        "end Main; end Program;\n",
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The same commitment on an ENUM receiver: §8.3's per-kind table gives an
// enum a default `Equals` (underlying integer equality) and `GetHashCode`,
// and §8.5 gives it a default `ToString` -- so the names are declared.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           enum_tostring_extension_does_not_bind) {
    expectRejected(
        "module R;\n"
        "public enum Color Red, Green; end Color;\n"
        "extension Color public method ToString(n: int): string => \"EXT\"; "
        "end;\n"
        "class Program public static method Main(): int\n"
        "  var c := Color.Red;\n"
        "  Console.WriteLine(c.ToString(3));\n"
        "  return 0;\n"
        "end Main; end Program;\n",
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The `extension Object` head reaches a user class too (§26.5 rule 2: a base
// type), and rule 1 still commits the NAME: `Object` declares `ToString`.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           object_head_extension_does_not_bind_on_a_user_class) {
    expectRejected(
        classProgram("extension Object public method ToString(n: int): "
                     "string => \"EXT\"; end;\n",
                     "  var x: C := new C(X := 5);\n"
                     "  Console.WriteLine(x.ToString(3));\n"),
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The committed member is the answer even when the extension's arity MATCHES
// the slot's: `Equals(other)` takes one argument, so `x.Equals(1)` is the
// `Object` slot's call -- the extension's `true` is not.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           class_equals_one_arg_extension_does_not_bind) {
    expectOut(
        classProgram("extension C public method Equals(other: int): bool => "
                     "true; end;\n",
                     "  var x: C := new C(X := 5);\n"
                     "  Console.WriteLine(x.Equals(1));\n"),
        "False\n");
}

// Likewise `GetHashCode()`: the identity hash is the answer, not the
// extension's.  The identity hash is drawn from a per-domain sequence
// (§8.3), so the row compares it against a bound rather than a literal.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           class_gethashcode_extension_does_not_bind) {
    expectOut(
        classProgram("extension C public method GetHashCode(): int => "
                     "123456789; end;\n",
                     "  var x: C := new C(X := 5);\n"
                     "  Console.WriteLine(x.GetHashCode() < 1000);\n"),
        "True\n");
}

// The receiver is evaluated ONCE: the extension's receiver and the `Object`
// slot's receiver were two separate evaluations of the same expression, so a
// side-effecting receiver ran twice.  With the extension unreachable the call
// is refused outright.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           bump_to_string_with_an_argument_is_rejected) {
    expectRejected(
        "module R;\n"
        "public class Box\n"
        "  public constructor(X: int) end constructor;\n"
        "  public method Bump(): Box\n"
        "    Console.WriteLine(\"bump\");\n"
        "    return this;\n"
        "  end Bump;\n"
        "end Box;\n"
        "extension Box public method ToString(n: int): string => \"EXT\"; "
        "end;\n"
        "class Program public static method Main(): int\n"
        "  var b: Box := new Box(X := 1);\n"
        "  Console.WriteLine(b.Bump().ToString(3));\n"
        "  return 0;\n"
        "end Main; end Program;\n",
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// ---- the control rows -------------------------------------------------------

// §8.1's universal members keep answering on a user class.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           class_object_slot_members_still_answer) {
    expectOut(classProgram("",
                            "  var x: C := new C(X := 5);\n"
                            "  var y: C := new C(X := 5);\n"
                            "  Console.WriteLine(x.ToString());\n"
                            "  Console.WriteLine(x.Equals(y));\n"),
              "R.C\nFalse\n");
}

// §26.5 rule 2: a name the receiver's declared type does NOT declare is
// still answered by the extension.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           a_name_the_class_does_not_declare_still_binds) {
    expectOut(classProgram("extension C public method Tag(): int => 42; "
                            "end;\n",
                            "  var x: C := new C(X := 5);\n"
                            "  Console.WriteLine(x.Tag());\n"),
              "42\n");
}

// §26.5 rule 1 read the ordinary way: a member the class DECLARES wins over
// a same-named extension (#433).
BEZEL_TEST(object_slot_rule1_user_type_7824,
           a_declared_member_wins_over_the_extension) {
    expectOut("module R;\n"
              "public class C\n"
              "  public constructor(X: int) end constructor;\n"
              "  public method ToString(n: int): string => \"DECL\";\n"
              "end C;\n"
              "extension C public method ToString(n: int): string => \"EXT\"; "
              "end;\n"
              "class Program public static method Main(): int\n"
              "  var x: C := new C(X := 5);\n"
              "  Console.WriteLine(x.ToString(3));\n"
              "  return 0;\n"
              "end Main; end Program;\n",
              "DECL\n");
}

// A record's own `ToString` (§8.5 / §9.1) still answers.
BEZEL_TEST(object_slot_rule1_user_type_7824, record_to_string_still_answers) {
    expectOut(recordProgram("",
                             "  var x: Rec := new Rec(X := 5);\n"
                             "  Console.WriteLine(x.ToString());\n"),
              "Rec(X: 5)\n");
}

// §26.5 rule 2: an `extension Object` block reaches a user class (`Object` is
// its base type, §8.1), so a name `Object` does NOT declare still binds.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           extension_object_still_applies_to_a_user_class) {
    expectOut(classProgram("extension Object public method Tag(): int => 42; "
                            "end;\n",
                            "  var x: C := new C(X := 5);\n"
                            "  Console.WriteLine(x.Tag());\n"),
              "42\n");
}

// The receiver of a surviving call is evaluated ONCE.
BEZEL_TEST(object_slot_rule1_user_type_7824, the_receiver_is_evaluated_once) {
    expectOut("module R;\n"
              "public class Box\n"
              "  public constructor(X: int) end constructor;\n"
              "  public method Bump(): Box\n"
              "    Console.WriteLine(\"bump\");\n"
              "    return this;\n"
              "  end Bump;\n"
              "end Box;\n"
              "class Program public static method Main(): int\n"
              "  var b: Box := new Box(X := 1);\n"
              "  Console.WriteLine(b.Bump().ToString());\n"
              "  return 0;\n"
              "end Main; end Program;\n",
              "bump\nR.Box\n");
}

// An interface receiver keeps the universal members (§8.1).
BEZEL_TEST(object_slot_rule1_user_type_7824,
           interface_object_slot_members_still_answer) {
    expectOut("module R;\n"
              "public interface IFoo\n"
              "  method Bar(): int;\n"
              "end IFoo;\n"
              "public class C: IFoo public method Bar(): int => 1; end C;\n"
              "class Program public static method Main(): int\n"
              "  var x: IFoo := new C();\n"
              "  Console.WriteLine(x.ToString());\n"
              "  return 0;\n"
              "end Main; end Program;\n",
              "R.C\n");
}

// An enum receiver keeps its own §8.3 / §8.5 members.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           enum_object_slot_members_still_answer) {
    expectOut("module R;\n"
              "public enum Color Red, Green; end Color;\n"
              "class Program public static method Main(): int\n"
              "  var c := Color.Red;\n"
              "  Console.WriteLine(c.ToString());\n"
              "  Console.WriteLine(c.Equals(Color.Red));\n"
              "  return 0;\n"
              "end Main; end Program;\n",
              "R.Color\nTrue\n");
}

// §26.5 rule 2 on an enum receiver: a name the enum does NOT declare is
// still answered by the extension.
BEZEL_TEST(object_slot_rule1_user_type_7824,
           a_name_the_enum_does_not_declare_still_binds) {
    expectOut("module R;\n"
              "public enum Color Red, Green; end Color;\n"
              "extension Color public method Tag(): int => 42; end;\n"
              "class Program public static method Main(): int\n"
              "  var c := Color.Red;\n"
              "  Console.WriteLine(c.Tag());\n"
              "  return 0;\n"
              "end Main; end Program;\n",
              "42\n");
}

// #8596 (spec §7.7, §6.6, §7.10, §5.1, v0.357; owner ruling on #8596,
// interpretation 2): the right operand of `??` / `??=` is ALWAYS type-checked,
// and a NON-nullable left operand draws a warning.
//
//   §7.7 *Typing*: "for a left operand of type `T?`, the right operand must be
//        implicitly convertible (§5.1) to `T` or to `T?`".
//   Ruling: for a left operand of non-nullable type `T`, `a ?? b` types exactly
//        as it would for `T?`; `x ??= e` follows through §6.6 ("with the typing
//        and ownership rules of §7.7"). The left is never null, so the right
//        operand never runs (§7.7 *Evaluation*): a WARNING, not an error (cf.
//        §31.2.3's covered `catch` arm).
//   §7.10 / §14.2: an unconstrained `T` is "treated conservatively as possibly
//        nullable"; `where T: notnull` / `class` / `value` / a type bound make
//        it non-nullable inside the body.
//
// MEASURED on the pre-fix base (r42-f18-6678 1405f6113): `var i: int := 1;
// i ??= "s";` and `i ?? "s"` compiled with no diagnostic (the right operand was
// never typed) and `s ??= "b"` on a `string` drew no warning.
//
// Every row here is red on the base: the rejects get their error, the warns get
// their warning. The quiet cases (a nullable left operand, a §7.4-narrowed
// binding, an unconstrained `T`) are green on both sides, so they are corpus
// candidates (lane contract §7), not gtest rows.

#include <string>
#include <string_view>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::Severity;

bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasDiag(const bezel::test::SemaResult& r, Severity sev,
             std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == sev && d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int countDiag(const bezel::test::SemaResult& r, Severity sev,
              std::string_view needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == sev && d.message.find(needle) != std::string::npos)
            ++n;
    return n;
}

const char* kAssignReject =
    "right operand of `\?\?=` has a type that is not implicitly convertible to "
    "the target type";
const char* kBinaryReject =
    "right operand of `\?\?` has a type that is not implicitly convertible to "
    "the target type";
const char* kAssignWarn =
    "the target of `\?\?=` has a non-nullable type, so its current value is "
    "never null: the right operand is never evaluated and the assignment never "
    "happens";
const char* kBinaryWarn =
    "the left operand of `\?\?` has a non-nullable type, so it is never null: "
    "the right operand is never evaluated";

std::string prog(const std::string& body, const std::string& members = "",
                 const std::string& generics = "") {
    return "module M;\n"
           "import System;\n"
           "public class Animal\n"
           "  public constructor() end constructor;\n"
           "end Animal;\n"
           "public class Dog: Animal\n"
           "  public constructor() end constructor;\n"
           "end Dog;\n"
           "public class Program\n" + members +
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n" + generics;
}

void expectRejected(const std::string& src, const char* needle) {
    auto r = semaAll(src);
    BEZEL_EXPECT_TRUE(hasDiag(r, Severity::Error, needle));
}

// A well-typed program that warns exactly once with `needle`.
void expectWarnsOnly(const std::string& src, const char* needle) {
    auto r = semaAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countDiag(r, Severity::Warning, needle), 1);
}

}  // namespace

// ---- Category A: the right operand converts to neither `T` nor `T?` ------

// The ruling's table: `i ??= "s"` on an `int` is a compile error.
BEZEL_TEST(coalesce_nonnullable_left_8596, int_assign_string_rejected_spec_6_6_7_7) {
    expectRejected(prog("    var i: int := 1;\n"
                        "    i ??= \"s\";\n"
                        "    Console.WriteLine(i);\n"),
                   kAssignReject);
}

// The ruling's table: `i ?? "s"` is a compile error.
BEZEL_TEST(coalesce_nonnullable_left_8596, int_coalesce_string_rejected_spec_7_7) {
    expectRejected(prog("    var i: int := 1;\n"
                        "    var j := i ?? \"s\";\n"
                        "    Console.WriteLine(j);\n"),
                   kBinaryReject);
}

// A non-nullable class left over an unrelated right operand.
BEZEL_TEST(coalesce_nonnullable_left_8596, class_coalesce_string_rejected_spec_7_7) {
    expectRejected(prog("    var a: Animal := new Animal();\n"
                        "    var b := a ?? \"s\";\n"
                        "    Console.WriteLine(b);\n"),
                   kBinaryReject);
}

// A `string` target assigned an `int`.
BEZEL_TEST(coalesce_nonnullable_left_8596, string_assign_int_rejected_spec_6_6_7_7) {
    expectRejected(prog("    var s: string := \"a\";\n"
                        "    s ??= 5;\n"
                        "    Console.WriteLine(s);\n"),
                   kAssignReject);
}

// A supertype right operand does not convert down to the subtype target.
BEZEL_TEST(coalesce_nonnullable_left_8596, subclass_assign_superclass_rejected_spec_6_6_7_7) {
    expectRejected(prog("    var d: Dog := new Dog();\n"
                        "    d ??= new Animal();\n"),
                   kAssignReject);
}

// A non-nullable field target.
BEZEL_TEST(coalesce_nonnullable_left_8596, field_assign_mismatch_rejected_spec_6_6_7_7) {
    expectRejected(prog("    var p := new Program();\n"
                        "    p._n ??= \"s\";\n",
                        "  public _n: int;\n"
                        "  public constructor() this._n := 0; end constructor;\n"),
                   kAssignReject);
}

// Even an unconstrained (possibly nullable) `T` types its right operand.
BEZEL_TEST(coalesce_nonnullable_left_8596, unconstrained_typeparam_coalesce_mismatch_rejected_spec_7_10) {
    expectRejected(
        prog("    Console.WriteLine(1);\n", "",
             "public class G\n"
             "  public static method F<T>(x: T): int\n"
             "    var r := x ?? 5;\n"
             "    return 0;\n"
             "  end F;\n"
             "end G;\n"),
        kBinaryReject);
}

// ---- Category B: well-typed, so a WARNING and no error --------------------

// The ruling's table: `s ??= "b"` on a `string` is well-typed but warns.
BEZEL_TEST(coalesce_nonnullable_left_8596, string_assign_string_warns_spec_6_6_7_7) {
    expectWarnsOnly(prog("    var s: string := \"a\";\n"
                         "    s ??= \"b\";\n"
                         "    Console.WriteLine(s);\n"),
                    kAssignWarn);
}

// The ruling's table: `i ?? 0` is well-typed but warns.
BEZEL_TEST(coalesce_nonnullable_left_8596, int_coalesce_int_warns_spec_7_7) {
    expectWarnsOnly(prog("    var i: int := 1;\n"
                         "    var j := i ?? 0;\n"
                         "    Console.WriteLine(j);\n"),
                    kBinaryWarn);
}

// `T? ` right operand converts to `T?`: still well-typed, still dead.
BEZEL_TEST(coalesce_nonnullable_left_8596, int_coalesce_nullable_int_warns_spec_7_7) {
    expectWarnsOnly(prog("    var i: int := 1;\n"
                         "    var k: int? := null;\n"
                         "    var j := i ?? k;\n"
                         "    Console.WriteLine(j);\n"),
                    kBinaryWarn);
}

// A `null` right operand converts to `T?`: well-typed, dead.
BEZEL_TEST(coalesce_nonnullable_left_8596, string_coalesce_null_warns_spec_7_7) {
    expectWarnsOnly(prog("    var s: string := \"a\";\n"
                         "    var t := s ?? null;\n"
                         "    Console.WriteLine(t);\n"),
                    kBinaryWarn);
}

// §5.1 widening: `int` converts to `long`.
BEZEL_TEST(coalesce_nonnullable_left_8596, long_assign_int_warns_spec_6_6_5_1) {
    expectWarnsOnly(prog("    var l: long := 1L;\n"
                         "    l ??= 2;\n"
                         "    Console.WriteLine(l);\n"),
                    kAssignWarn);
}

// A non-binding left operand: its static type decides (a call result).
BEZEL_TEST(coalesce_nonnullable_left_8596, call_operand_warns_spec_7_7) {
    expectWarnsOnly(prog("    var j := Program.Five() ?? 0;\n"
                         "    Console.WriteLine(j);\n",
                         "  public static method Five(): int return 5; end Five;\n"),
                    kBinaryWarn);
}

// A non-nullable field target.
BEZEL_TEST(coalesce_nonnullable_left_8596, field_assign_warns_spec_6_6_7_7) {
    expectWarnsOnly(prog("    var p := new Program();\n"
                         "    p._n ??= 3;\n",
                         "  public _n: int;\n"
                         "  public constructor() this._n := 0; end constructor;\n"),
                    kAssignWarn);
}

// §7.10: `where T: notnull` makes T non-nullable inside the body.
BEZEL_TEST(coalesce_nonnullable_left_8596, notnull_typeparam_warns_spec_7_10) {
    expectWarnsOnly(
        prog("    Console.WriteLine(1);\n", "",
             "public class G\n"
             "  public static method F<T>(x: T, d: T): T where T: notnull\n"
             "    return x ?? d;\n"
             "  end F;\n"
             "end G;\n"),
        kBinaryWarn);
}

// §7.10: `where T: class` likewise.
BEZEL_TEST(coalesce_nonnullable_left_8596, class_typeparam_warns_spec_7_10) {
    expectWarnsOnly(
        prog("    Console.WriteLine(1);\n", "",
             "public class G\n"
             "  public static method F<T>(x: T, d: T): T where T: class\n"
             "    return x ?? d;\n"
             "  end F;\n"
             "end G;\n"),
        kBinaryWarn);
}

// §7.10: `where T: value` likewise.
BEZEL_TEST(coalesce_nonnullable_left_8596, value_typeparam_warns_spec_7_10) {
    expectWarnsOnly(
        prog("    Console.WriteLine(1);\n", "",
             "public class G\n"
             "  public static method F<T>(x: T, d: T): T where T: value\n"
             "    return x ?? d;\n"
             "  end F;\n"
             "end G;\n"),
        kBinaryWarn);
}

// §7.10: a type bound ("Inside the generic body, T is non-nullable under any
// type-bound constraint"), and `??=` through a `notnull` local.
BEZEL_TEST(coalesce_nonnullable_left_8596, type_bound_typeparam_assign_warns_spec_7_10) {
    expectWarnsOnly(
        prog("    Console.WriteLine(1);\n", "",
             "public class G\n"
             "  public static method F<T>(x: T, d: T): T where T: Animal\n"
             "    var y: T := x;\n"
             "    y ??= d;\n"
             "    return y;\n"
             "  end F;\n"
             "end G;\n"),
        kAssignWarn);
}

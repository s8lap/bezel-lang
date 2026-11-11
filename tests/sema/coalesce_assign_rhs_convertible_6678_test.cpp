// #6678 (spec §6.6, §7.7, §5.1, v0.357): the right operand of `??=` is typed by
// §7.7's rule.
//
//   §6.6: "`x ??= e` assigns `e` only when `x`'s current value is null, with
//          the typing and ownership rules of §7.7."
//   §7.7: "for a left operand of type `T?`, the right operand must be
//          implicitly convertible (§5.1) to `T` or to `T?`".
//
// MEASURED on the pre-fix base (r30-c1 a44c7bec8): `var s: string? := null;
// s ??= 5; Console.WriteLine(s);` compiled with no diagnostic and the binary
// died with SIGSEGV (the `int` was stored into the string slot as a pointer).
// The sibling shapes were accepted too and stored garbage: `int? ??= "s"`
// printed 0, `Animal? ??= "str"` and `Dog? ??= new Animal()` ran. #6214 added
// the §7.7 check to the binary `??`; the compound form's statement arm never
// ran it.
//
// Category A rejects each non-convertible right operand at the §5.1
// conversion site. Category C is §5.1's "Constant narrowing" at this position:
// the check re-types an in-range constant to the target's integer type, as
// `:=` does, so `byte? ??= 50` and `int? ??= 5L` store the value (MEASURED on
// the base: both were accepted and printed 0).
//
// Every row here is red on the base. The ADMITTING TWINS on the same shapes
// (identity, §5.1 widening, the nullable lift, a subtype, `null`, a nullable
// right operand, a lambda at a function-typed target, member and indexer
// targets) compile and print the same value on the base and with the fix, so
// they are corpus candidates, not gtest rows (lane contract §7).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kNeedle =
    "right operand of `\?\?=` has a type that is not implicitly convertible to "
    "the target type (\xC2\xA7""5.1)";

std::string prog(const std::string& body, const std::string& members = "") {
    return "module M;\n"
           "import System;\n"
           "public class Animal\n"
           "  public constructor() end constructor;\n"
           "  public virtual method Name(): string\n"
           "    return \"animal\";\n"
           "  end Name;\n"
           "end Animal;\n"
           "public class Dog: Animal\n"
           "  public constructor() end constructor;\n"
           "  public override method Name(): string\n"
           "    return \"dog\";\n"
           "  end Name;\n"
           "end Dog;\n"
           "public class Program\n" + members +
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}

const char* kInstanceMembers =
    "  private _s: string?;\n"
    "  private _n: int?;\n"
    "  public property P: string?;\n"
    "  public constructor()\n"
    "    this._s := null;\n"
    "    this._n := null;\n"
    "    this.P := null;\n"
    "  end constructor;\n";

void expectRejected(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "IR verification"));
}

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// Category A — the right operand converts to neither `T` nor `T?`.

// The issue's repro verbatim. Pre-fix: accepted, the binary SIGSEGVs.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, issue_repro_string_nullable_assign_int_rejected_spec_7_7) {
    expectRejected(prog("    var s: string? := null;\n"
                        "    s ??= 5;\n"
                        "    Console.WriteLine(s);\n"));
}

// Pre-fix: accepted, printed 0.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, int_nullable_assign_string_rejected_spec_7_7) {
    expectRejected(prog("    var i: int? := null;\n"
                        "    i ??= \"s\";\n"
                        "    Console.WriteLine(i ?? 0);\n"));
}

BEZEL_TEST(coalesce_assign_rhs_convertible_6678, class_nullable_assign_string_rejected_spec_7_7) {
    expectRejected(prog("    var a: Animal? := null;\n"
                        "    a ??= \"str\";\n"
                        "    Console.WriteLine(a is null);\n"));
}

// A supertype is not convertible down to the target's type.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, subclass_nullable_assign_base_rejected_spec_7_7) {
    expectRejected(prog("    var d: Dog? := null;\n"
                        "    d ??= new Animal();\n"
                        "    Console.WriteLine(d is null);\n"));
}

// §5.1's table has no `long` -> `int` row; a run-time value never narrows.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, int_nullable_assign_long_variable_rejected_spec_5_1) {
    expectRejected(prog("    var i: int? := null;\n"
                        "    var l: long := 7L;\n"
                        "    i ??= l;\n"
                        "    Console.WriteLine(i ?? 0);\n"));
}

// The nullable lift needs the underlying conversion: `int?` does not convert
// to `string?`. Pre-fix: accepted, the binary SIGSEGVs.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, string_nullable_assign_int_nullable_rejected_spec_5_1) {
    expectRejected(prog("    var s: string? := null;\n"
                        "    var n: int? := 4;\n"
                        "    s ??= n;\n"
                        "    Console.WriteLine(s);\n"));
}

// The target's DECLARED type decides, also where §7.4 has narrowed it.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, narrowed_string_nullable_assign_int_rejected_spec_7_7) {
    expectRejected(prog("    var s: string? := \"a\";\n"
                        "    s ??= 5;\n"
                        "    Console.WriteLine(s);\n"));
}

// Member targets: an instance field (qualified and bare) and an auto-property.
// Pre-fix: all three accepted; the two `string?` targets SIGSEGV at run time,
// the `int?` one printed 0.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, this_field_string_nullable_assign_int_rejected_spec_7_7) {
    expectRejected(prog("    var p := new Program();\n"
                        "    p.Bad();\n",
                        std::string(kInstanceMembers) +
                        "  public method Bad()\n"
                        "    this._s ??= 5;\n"
                        "    Console.WriteLine(this._s ?? \"none\");\n"
                        "  end Bad;\n"));
}

BEZEL_TEST(coalesce_assign_rhs_convertible_6678, bare_field_int_nullable_assign_string_rejected_spec_7_7) {
    expectRejected(prog("    var p := new Program();\n"
                        "    p.Bad();\n",
                        std::string(kInstanceMembers) +
                        "  public method Bad()\n"
                        "    _n ??= \"x\";\n"
                        "    Console.WriteLine(_n ?? 0);\n"
                        "  end Bad;\n"));
}

BEZEL_TEST(coalesce_assign_rhs_convertible_6678, property_string_nullable_assign_int_rejected_spec_7_7) {
    expectRejected(prog("    var p := new Program();\n"
                        "    p.P ??= 5;\n"
                        "    Console.WriteLine(p.P ?? \"none\");\n",
                        kInstanceMembers));
}

// §5.1 "Constant narrowing": "implicitly converts to any integer type whose
// range contains its value" — 300 is outside `byte`'s. Pre-fix: accepted,
// printed 1.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, byte_nullable_assign_out_of_range_constant_rejected_spec_5_1) {
    expectRejected(prog("    var b: byte? := null;\n"
                        "    b ??= 300;\n"
                        "    Console.WriteLine(b ?? 0);\n"));
}

// Indexer targets. Pre-fix: the dictionary row died in IR verification with
// no sema diagnostic, the array row was accepted and printed -1.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, dictionary_index_string_nullable_assign_int_rejected_spec_7_7) {
    expectRejected(prog("    var d := new Dictionary<int, string?>();\n"
                        "    d[1] := null;\n"
                        "    d[1] ??= 5;\n"
                        "    Console.WriteLine((d[1] ?? \"none\").Length);\n"));
}

BEZEL_TEST(coalesce_assign_rhs_convertible_6678, array_index_int_nullable_assign_string_rejected_spec_7_7) {
    expectRejected(prog("    var a := new Array<int?>(2, null);\n"
                        "    a[0] ??= \"s\";\n"
                        "    Console.WriteLine(a[0] ?? -1);\n"));
}

// Category C — §5.1 "Constant narrowing" ("the source may be a compile-time
// constant of any integer type — not only `int` — and the target any integer
// type whose range contains the value"). Pre-fix: both printed 0.
BEZEL_TEST(coalesce_assign_rhs_convertible_6678, in_range_constant_narrows_to_the_target_spec_5_1) {
    expectPrints(prog("    var b: byte? := null;\n"
                      "    b ??= 50;\n"
                      "    Console.WriteLine(b ?? 0);\n"
                      "    var i: int? := null;\n"
                      "    i ??= 5L;\n"
                      "    Console.WriteLine(i ?? 0);\n"),
                 "50\n5\n");
}

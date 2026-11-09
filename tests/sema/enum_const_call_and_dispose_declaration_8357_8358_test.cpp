// Regression tests for issues #8357 and #8358 — the two ADJACENT members of
// the "a member name through a type name" family that the compiler admits and
// must refuse.
//
//   #8358 — a CALL whose callee resolves to an ENUM CONSTANT (`E.Green()`).
//     §26.5 rule 2 (v0.355): "Through a type name (§2.7 rule 1) the chain
//     resolves against the type's nested types, enum constants and static
//     members, declared or inherited". So `E.Green` RESOLVES — to the enum
//     CONSTANT — and §10.1 (v0.355) makes an enum constant a VALUE of the
//     enum type. §10.2 (v0.355) gives it its underlying integer at the
//     DECLARATION (`Ok := 200`) and §5.4's enum cast `E(n)` is spelled on the
//     TYPE NAME, so there is NO call form for an enum constant in this
//     language — neither an argument-free one nor one carrying the underlying
//     value. A call of an enum constant therefore names no method.
//
//   #8357 — an ENUM CONSTANT or a CHOICE CASE named `Dispose`, refused AT ITS
//     DECLARATION. §28.2 (v0.355): "Declaring a member named `Dispose` on a
//     class, a record, an enum or a choice is a compile error at that
//     declaration, whatever its parameter list and whatever its kind — a
//     method, instance or `static`, a field, a property, a `const`, AN ENUM
//     CONSTANT, A CHOICE CASE or a nested type".
//
// THE CHOICE SIDE OF BOTH FAMILIES IS ALREADY REFUSED, and the controls below
// pin that: the payload-free case call by §35.3 / §35.5 ("a payload-free case
// is the value `Ch.Dispose`, not a call"), and the `Dispose`-named call by
// #7927 / #7934. The enum side of the CALL family had no counterpart at all,
// and the DECLARATION gate (the #7517 check) refused a member of that name
// only when the member was a MethodSymbol — an EnumConstantSymbol and a
// ChoiceCaseSymbol are member symbols of another kind and passed it.
//
// THE CONTROLS (they must stay exactly as they are):
//   · the enum constant READ `E.Green` — a value, admitted (§26.5 rule 2);
//   · a binding over the constant `var f := E.Green;` — admitted;
//   · a class's and a record's `Dispose` METHOD — refused at the declaration;
//   · an INTERFACE declaring `method Dispose();` — ADMITTED (§28.2's last
//     paragraph: "Interfaces are unaffected");
//   · a choice's payload-free case call — refused by §35.3 / §35.5;
//   · a choice's payload case call — admitted with the right arguments and
//     refused with the wrong ones;
//   · a STATIC member through a type name and a NESTED TYPE read through a
//     type name — admitted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME (not crashed, not
// hung), and a diagnostic carrying the expected TEXT is among them.
bool rejectedWith(const std::string& program, const char* text) {
    JitResult r = runAot(program);
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, text);
}

// The stricter oracle: the diagnostic carrying the expected TEXT sits at the
// expected REAL LOCATION — the oracle the DECLARATION rows need (§28.2: "a
// compile error AT THAT DECLARATION").
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle for a program that must COMPILE (and run).
void expectCompiles(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

const char* kMain = "public class Program";
const char* kMainBody = "  public static method Main()";
const char* kMainPrint = "    Console.WriteLine(\"hi\");";
const char* kMainEnd = "  end Main;";

// The text #8358's diagnostic carries: it names the enum AND the constant.
const char* kEnumConstCallText = "is a value, not a method";

// The text #8357's two diagnostics carry: they name the kind, the type and
// the member.
const char* kEnumConstDisposeText =
    "may not declare an enum constant named `Dispose`";
const char* kChoiceCaseDisposeText =
    "may not declare a case named `Dispose`";

}  // namespace

// ---------------------------------------------------------------------------
// #8358 — the CALL of an enum constant.
// ---------------------------------------------------------------------------

// REPRO (red on the base, green on the tip): `E.Green()` names the enum
// CONSTANT — a value — and there is no method to call.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_call_is_rejected_spec_26_5_rule_2_10_1) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    E.Green();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstCallText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "the enum constant `E.Green`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""26.5 rule 2"));
}

// REPRO: the same call WITH an argument. An enum constant's underlying value
// is written at its DECLARATION (`Ok := 200`, §10.2) and the enum cast
// `E(n)` is spelled on the TYPE NAME (§5.4), so there is no argument-list
// form for a constant either — the rule is the blanket one, and this row
// pins that the argument does not open one.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_call_with_an_argument_is_rejected_spec_26_5_rule_2_10_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    E.Green(1);",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstCallText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "the enum constant `E.Green`"));
}

// REPRO: the same call through a type ALIAS of the enum.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_call_through_an_alias_is_rejected_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        "type EA := E;",
        kMain,
        kMainBody,
        "    EA.Green();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstCallText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "the enum constant `EA.Green`"));
}

// REPRO: the same call on a NESTED enum — the receiver of the call is the
// dotted type name `Container.Status`.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           nested_enum_constant_call_is_rejected_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public class Container",
        "  public enum Status Green; end Status;",
        "end Container;",
        kMain,
        kMainBody,
        "    Container.Status.Green();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstCallText));
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "the enum constant `Container.Status.Green`"));
}

// CONTROL: the enum constant READ — a value, not a call (§26.5 rule 2). The
// #8358 check is a CALL check and leaves the read alone.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_read_stays_admitted_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    E.Green;",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: the same constant READ into a binding — the READ spelling, not a
// call.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_binding_stays_admitted_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    var f := E.Green;",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: the constant READ through a variable of the enum type.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_read_through_a_variable_stays_admitted_spec_10_1) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    var e: E := E.Green;",
        "    if e = E.Green then Console.WriteLine(\"ok\"); end;",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// ---------------------------------------------------------------------------
// #8357 — the DECLARATION of an enum constant / a choice case named `Dispose`.
// ---------------------------------------------------------------------------

// REPRO (red on the base, green on the tip): the enum constant named
// `Dispose` is refused AT ITS DECLARATION (§28.2).
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_named_dispose_is_rejected_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Dispose, Green; end E;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstDisposeText));
    // The diagnostic names the enum AND the constant.
    BEZEL_EXPECT_TRUE(rejectedWith(p, "enum `E`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""28.2"));
}

// REPRO: the CHOICE twin — a case named `Dispose`, refused AT ITS
// DECLARATION.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           choice_case_named_dispose_is_rejected_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public choice Ch Dispose, B; end Ch;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kChoiceCaseDisposeText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "choice `Ch`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""28.2"));
}

// REPRO: the enum constant named `Dispose` on an enum that also declares
// other constants — the diagnostic names the enum AND the constant, at the
// constant's own declaration.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_constant_named_dispose_among_others_is_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public enum E",
        "  Red, Dispose, Green;",
        "end E;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kEnumConstDisposeText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "enum `E`"));
}

// REPRO: a choice case named `Dispose` that CARRIES A PAYLOAD — the
// declaration is still the error (§28.2: "whatever its parameter list and
// whatever its kind").
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           choice_case_named_dispose_with_a_payload_is_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public choice Ch",
        "  Dispose(v: int),",
        "  B;",
        "end Ch;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, kChoiceCaseDisposeText));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "choice `Ch`"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS — every one of these is UNCHANGED by this fix.
// ---------------------------------------------------------------------------

// CONTROL: a class's `Dispose` METHOD is still refused at its declaration,
// with the class's own diagnostic.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           class_dispose_method_stays_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public method Dispose()",
        "  end Dispose;",
        "end K;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "class `K` may not declare a `Dispose` method directly "
           "(\xc2\xa7""28.2)"));
}

// CONTROL: a record's `Dispose` METHOD is still refused at its declaration.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           record_dispose_method_stays_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public record R(X: int)",
        "  public method Dispose()",
        "  end Dispose;",
        "end R;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "record `R` may not declare a `Dispose` method directly "
           "(\xc2\xa7""28.2)"));
}

// CONTROL: an INTERFACE declaring `method Dispose();` stays ADMITTED —
// §28.2's last paragraph: "Interfaces are unaffected — the rule reaches
// classes, records, enums and choices only".
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           interface_dispose_method_stays_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public interface I",
        "  method Dispose();",
        "end I;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: an enum and a choice that declare NO `Dispose` stay admitted.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_and_choice_without_a_dispose_member_stay_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Red, Green; end E;",
        "public choice Ch A(v: int), B; end Ch;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: the CHOICE payload-free case call is still refused by §35.3 /
// §35.5 — the choice side of the family is unchanged. The case is named
// `Bare` here so that the row measures the ARITY rule and not the §28.2
// declaration rule (#8357's own shape).
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           choice_payload_free_case_call_stays_refused_spec_35_3_35_5) {
    const std::string p = prog({
        "module M;",
        "public choice Ch Bare, B; end Ch;",
        kMain,
        kMainBody,
        "    Ch.Bare();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "case `Ch.Bare` has no payload, so it takes no argument list"));
}

// CONTROL: a CHOICE case WITH a payload, called with the right arguments,
// stays ADMITTED.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           choice_payload_case_call_with_the_right_arguments_stays_admitted_spec_35_3) {
    const std::string p = prog({
        "module M;",
        "public choice Ch",
        "  With(v: int),",
        "  Bare;",
        "end Ch;",
        kMain,
        kMainBody,
        "    var c := Ch.With(3);",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: a CHOICE case WITH a payload, called with the WRONG arguments,
// stays refused.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           choice_payload_case_call_with_the_wrong_arguments_stays_refused_spec_35_3) {
    const std::string p = prog({
        "module M;",
        "public choice Ch",
        "  With(v: int),",
        "  Bare;",
        "end Ch;",
        kMain,
        kMainBody,
        "    var c := Ch.With(\"nope\");",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "the argument `v` to case `Ch.With`"));
}

// CONTROL: a STATIC member of a class called through the class name stays
// ADMITTED (§26.5 rule 2 supplies the static members).
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           static_method_through_a_type_name_stays_admitted_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        kMain,
        kMainBody,
        "    K.Real();",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: a NESTED TYPE read through a type name stays ADMITTED (§26.5
// rule 2 supplies the nested types).
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           nested_type_read_through_a_type_name_stays_admitted_spec_26_5_rule_2) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public class Inner",
        "  end Inner;",
        "end K;",
        kMain,
        kMainBody,
        "    var x: K.Inner := new K.Inner();",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: the enum cast `E(n)` — §5.4's spelling of a value of the enum
// type — stays ADMITTED. It is spelled on the TYPE NAME, so it is not the
// call of an enum constant.
BEZEL_TEST(enum_const_call_dispose_decl_8357_8358,
           enum_cast_through_the_type_name_stays_admitted_spec_5_4) {
    const std::string p = prog({
        "module M;",
        "public enum E Green; end E;",
        kMain,
        kMainBody,
        "    var e: E := E(0);",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// Regression tests for issue #7934 — two sema defects on the `Dispose` name:
//
//   (1) `E.Dispose();` compiled when the enum `E` DECLARED a case named
//       `Dispose`. The call names an enum CONSTANT — a value — and §8.7
//       makes `Dispose()` a member of a class type and of nothing else;
//       §28.2 (v0.355) forbids the name on an enum in every member kind, the
//       constant included.
//
//   (2) `KA.Dispose();` compiled when `KA` is a `type KA := K;` alias of a
//       class that declares no such static member. §26.5 rule 2 (v0.355):
//       "Through a type name the chain resolves against the type's nested
//       types, enum constants and static members, declared or inherited — and
//       a call or read `T.M` that none of them supplies names no member and
//       is the same compile error: an instance method of the name does not
//       supply it, there being no receiver". `Dispose` is not a static member
//       of any type (the compiler GENERATES it on the class's INSTANCE
//       surface, and §28.2 forbids a class from declaring one), so no type
//       name supplies it.
//
// THE CONTROLS (the admitting twins — they must stay admitted):
//   · a legal case READ (`E.Green;`, and the read of the constant named
//     `Dispose` itself: `E.Dispose;`) — §26.5 rule 2 resolves a type name's
//     chain against the type's enum constants;
//   · a STATIC member through a type name and through an alias
//     (`K.Real();`, `KA.Real();`);
//   · `Dispose()` on a VALUE of a class — the generated instance member;
//   · a STATIC interface member named `Dispose` — §11.8's static interface
//     members DO resolve through a type name.

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

// The oracle: the program was REJECTED AT COMPILE TIME, and a diagnostic
// carrying the expected TEXT is among them.
bool rejectedWith(const std::string& program, const char* text) {
    JitResult r = runAot(program);
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, text);
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

// The enum of (1): a case named `Dispose` beside an ordinary one.
const char* kEnumWithDisposeCase[] = {
    "public enum E",
    "  Dispose, Green;",
    "end E;",
};

}  // namespace

// (1) `E.Dispose()` on an enum that declares a case named `Dispose`: the call
// names a VALUE (the enum constant), not a `Dispose` member.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           enum_case_named_dispose_type_name_call_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        kEnumWithDisposeCase[0],
        kEnumWithDisposeCase[1],
        kEnumWithDisposeCase[2],
        kMain,
        kMainBody,
        "    E.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "`Dispose` is not admitted on an enum value"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "the enum constant `E.Dispose` is a value, not a `Dispose` member"));
}

// (1) the same call through a `type` ALIAS of that enum. The diagnostic
// names the access AS WRITTEN (`EA.Dispose`) — the alias and the enum are one
// identity under §27.8, and the spelling is the one in the source.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           enum_case_named_dispose_through_an_alias_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        kEnumWithDisposeCase[0],
        kEnumWithDisposeCase[1],
        kEnumWithDisposeCase[2],
        "type EA := E;",
        kMain,
        kMainBody,
        "    EA.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "`Dispose` is not admitted on an enum value"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "the enum constant `EA.Dispose` is a value, not a `Dispose` member"));
}

// (2) `KA.Dispose()` where `KA` is a `type KA := K;` alias of a class that
// declares no such static member.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           class_alias_type_name_dispose_call_is_rejected_spec_26_5_8_7) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        "type KA := K;",
        kMain,
        kMainBody,
        "    KA.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `K`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""26.5 rule 2"));
}

// (2) the same call on the class type name itself (no alias): the alias is
// not what makes the name resolve — §26.5 rule 2 is about the type name.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           class_type_name_dispose_call_is_rejected_spec_26_5_8_7) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        kMain,
        kMainBody,
        "    K.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `K`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""26.5 rule 2"));
}

// (2) the READ spelling (`KA.Dispose` as a bare expression) is a §27.6
// method-reference FORMATION, not an access: §27.6's own rule for this name
// ("No method reference to `Dispose` or `DisposeAsync` ... type-qualified
// (`D.Dispose`, `IDisposable.Dispose`) — is a compile error at the formation")
// governs it, and that rule is tracked by #7344; the read half of §26.5
// rule 2 against that formation rule is the open spec-question #8065. This
// fix answers neither, so the READ form is deliberately NOT covered here —
// asserting it either way would pin a behaviour this fix does not own.

// (2) the same call on a RECORD type name: a record has no `Dispose` member
// at all (§8.7 — records do not implement IDisposable), so no type name
// supplies it either.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           record_alias_type_name_dispose_call_is_rejected_spec_26_5_8_7) {
    const std::string p = prog({
        "module M;",
        "public record R(X: int);",
        "type RA := R;",
        kMain,
        kMainBody,
        "    RA.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `R`"));
}

// (2) the same call on an INTERFACE type name whose interface declares no
// `Dispose` at all: §8.7 makes `Dispose()` a member of an interface that
// declares `method Dispose();` itself — an interface that declares none has
// no `Dispose` member, so its type name supplies none either.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           interface_alias_type_name_dispose_call_is_rejected_spec_26_5_8_7) {
    const std::string p = prog({
        "module M;",
        "public interface I",
        "  method Foo();",
        "end I;",
        "type IA := I;",
        kMain,
        kMainBody,
        "    IA.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `I`"));
}

// (1) the same defect through a NESTED type name: the receiver of the call
// is the dotted type name `Container.Status`, and the enum constant named
// `Dispose` is just as much a value there as in the top-level spelling.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           nested_enum_case_named_dispose_type_name_call_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Container",
        "  public enum Status Dispose, Green; end Status;",
        "end Container;",
        kMain,
        kMainBody,
        "    Container.Status.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "`Dispose` is not admitted on an enum value"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "the enum constant `Container.Status.Dispose` is a value, not a "
           "`Dispose` member"));
}

// CONTROL (FLIPPED by #8357, §28.2): the enum constant named `Dispose` is
// NO LONGER DECLARABLE — §28.2 (v0.355) forbids the name on an enum "whatever
// its parameter list and whatever its kind — a method, instance or `static`,
// a field, a property, a `const`, AN ENUM CONSTANT, a choice case or a nested
// type" — so the program is refused AT THE DECLARATION and the READ never
// comes up. The read of an ADMISSIBLE enum constant through a type name stays
// admitted (§26.5 rule 2); see
// enum_case_read_through_the_type_name_stays_admitted_spec_26_5_rule_2.
//
// The nesting depth is the point of this row: the §28.2 declaration refusal
// reaches a nested enum exactly as it reaches a module-level one.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           nested_enum_case_named_dispose_is_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Container",
        "  public enum Status Dispose, Green; end Status;",
        "end Container;",
        kMain,
        kMainBody,
        "    var x := Container.Status.Dispose;",
        "    if x = Container.Status.Dispose then Console.WriteLine(\"ok\"); end;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "may not declare an enum constant named `Dispose`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""28.2"));
}

// CONTROL (FLIPPED by #8357, §28.2): the enum constant named `Dispose` is NO
// LONGER DECLARABLE — §28.2 (v0.355) forbids the name on an enum in every
// member kind, the constant included, so the error is AT THE DECLARATION and
// the READ is never reached. (Before #8357 this row asserted the opposite:
// that the constant was declarable and therefore readable. The §28.2 text is
// what decides it — the DECLARATION is the error.)
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           enum_case_named_dispose_is_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        kEnumWithDisposeCase[0],
        kEnumWithDisposeCase[1],
        kEnumWithDisposeCase[2],
        kMain,
        kMainBody,
        "    var x := E.Dispose;",
        "    if x = E.Dispose then Console.WriteLine(\"ok\"); end;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "may not declare an enum constant named `Dispose`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "enum `E`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""28.2"));
}

// CONTROL: an ordinary case of an enum reads the same way — §26.5 rule 2
// resolves a type name's chain against the type's enum constants, and the
// read is a value read.
//
// (FIXTURE, #8357): the enum here declares no constant named `Dispose`. It
// used to declare one beside `Green`, and the row's claim — that an ordinary
// case reads the same way — was then true only of a program §28.2 forbids
// (the declaration of that constant is now an error, #8357), so the whole
// program was refused and the read never came up. The row's claim is about
// the ORDINARY case; the ordinary case needs an ordinary enum.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           enum_case_read_through_the_type_name_stays_admitted_spec_26_5_rule_2) {
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

// CONTROL: a STATIC member resolves through a type name and through an alias
// (§26.5 rule 2 / §22.12) — the type-name rule does not reject those.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           static_member_through_a_type_name_and_an_alias_stays_admitted_spec_22_12) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        "type KA := K;",
        kMain,
        kMainBody,
        "    K.Real();",
        "    KA.Real();",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: `Dispose()` on a VALUE of a class is the compiler-generated
// instance member (§8.7) and stays admitted.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           dispose_on_a_class_value_stays_admitted_spec_8_7) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public method Hi()",
        "    Console.WriteLine(\"hi\");",
        "  end Hi;",
        "end K;",
        kMain,
        kMainBody,
        "    var k: K := new K();",
        "    k.Dispose();",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: a STATIC interface member named `Dispose` DOES resolve through the
// interface's type name (§11.8) — only the instance member does not.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           static_interface_member_named_dispose_stays_admitted_spec_11_8) {
    const std::string p = prog({
        "module M;",
        "public interface I",
        "  static method Dispose();",
        "end I;",
        kMain,
        kMainBody,
        "    I.Dispose();",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// CONTROL: an unknown member through a type name keeps its own diagnostic
// (§11.3 / §12.1) — the `Dispose` rule does not change it.
BEZEL_TEST(enum_case_dispose_and_type_name_static_7934,
           unknown_member_through_a_type_name_keeps_its_own_diagnostic_spec_11_3) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        "type KA := K;",
        kMain,
        kMainBody,
        "    KA.Bogus();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Bogus` on type `K`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""11.3, \xc2\xa7""12.1"));
}

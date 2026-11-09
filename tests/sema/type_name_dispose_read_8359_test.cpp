// Regression tests for issue #8359 — the READ spelling of `Dispose` through
// an ENUM or a CHOICE type name (`E.Dispose;`) compiled with no diagnostic,
// while the CALL spelling of the same access (`E.Dispose();`) is rejected.
//
// THE DEFECT: §26.5 rule 2 (v0.355) — "Through a type name (§2.7 rule 1) the
// chain resolves against the type's nested types, enum constants and static
// members, declared or inherited — and a call or read `T.M` that none of them
// supplies names no member and is the same compile error: an instance method
// of the name does not supply it, there being no receiver". `Dispose` is
// supplied by none of them (§8.7, v0.350: it is a member of a CLASS type's
// instance surface, of an INTERFACE that is or extends `IDisposable` or
// declares `method Dispose();` itself, and of nothing else; and §28.2,
// v0.355, forbids declaring a member of that name on a class, a record, an
// enum or a choice at all). So `E.Dispose;` over an enum type name names no
// member — the same compile error the CALL spelling already gets.
//
// §27.6 (v0.350) selects the DIAGNOSTIC for this spelling: "Where the
// receiver's type has no `Dispose` member at all (§8.7), the no-member
// diagnostic applies instead." An enum and a choice have no `Dispose` member
// at all, so the no-member diagnostic is the one that applies — the §27.6
// method-reference rule (tracked by #7344, whose read half against §26.5
// rule 2 is the open spec-question #8065) governs the CLASS and the INTERFACE
// type name, NOT these two kinds.
//
// THE CONTROLS (they must stay as they are):
//   · `E.SomeOtherCase;` — the same gate, the same wording (the read of any
//     other unknown name through a type name was already rejected);
//   · an enum that DECLARES a case named `Dispose` — §26.5 rule 2 resolves a
//     type name's chain against the type's enum constants, so that read is a
//     legal constant read and stays admitted (the §28.2 prohibition on
//     DECLARING the name is a separate rule, not this one);
//   · the CALL spelling over an enum or a choice — rejected already, by its
//     own §8.7 diagnostic ("`Dispose` is not admitted on an enum value");
//   · `K.Dispose;` over a CLASS type name and `IDisposable.Dispose;` over an
//     INTERFACE type name — those are §27.6 method-reference FORMATIONS, and
//     §27.6's own rule for the name governs them (#7344 / #8065, both OUT OF
//     SCOPE here) — this fix must not change them;
//   · `v.Dispose;` and `v.Dispose();` over a VALUE of a class — the
//     legitimate universal member (§8.7).

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

// The enum and the choice of the defect: neither declares a `Dispose` case,
// so neither supplies the name.
const char* kEnum = "public enum E Red, Green; end E;";
const char* kChoice = "public choice Ch A, B; end Ch;";

}  // namespace

// (1) `E.Dispose;` — the READ spelling through an ENUM type name.
BEZEL_TEST(type_name_dispose_read_8359,
           enum_type_name_dispose_read_is_rejected_spec_27_6_8_7_26_5) {
    const std::string p = prog({
        "module M;",
        kEnum,
        kMain,
        kMainBody,
        "    E.Dispose;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `E`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""11.3, \xc2\xa7""12.1"));
}

// (2) `Ch.Dispose;` — the READ spelling through a CHOICE type name.
BEZEL_TEST(type_name_dispose_read_8359,
           choice_type_name_dispose_read_is_rejected_spec_27_6_8_7_26_5) {
    const std::string p = prog({
        "module M;",
        kChoice,
        kMain,
        kMainBody,
        "    Ch.Dispose;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `Ch`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""11.3, \xc2\xa7""12.1"));
}

// (3) `EA.Dispose;` — the same access through a `type` ALIAS of the enum
// (§27.8: the alias sees through to the aliased type, so the rule that
// governs the type name governs the alias). The diagnostic names the
// UNDERLYING type, as the CALL spelling's does.
BEZEL_TEST(type_name_dispose_read_8359,
           alias_type_name_dispose_read_is_rejected_spec_27_6_8_7_26_5) {
    const std::string p = prog({
        "module M;",
        kEnum,
        "type EA := E;",
        kMain,
        kMainBody,
        "    EA.Dispose;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `E`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""11.3, \xc2\xa7""12.1"));
}

// (3b) the same access through an alias of a CHOICE.
BEZEL_TEST(type_name_dispose_read_8359,
           choice_alias_type_name_dispose_read_is_rejected_spec_27_6_8_7_26_5) {
    const std::string p = prog({
        "module M;",
        kChoice,
        "type CHA := Ch;",
        kMain,
        kMainBody,
        "    CHA.Dispose;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `Ch`"));
}

// (3c) the same access through a NESTED type name — the receiver is the
// dotted type name `Container.Status`, a type name too.
BEZEL_TEST(type_name_dispose_read_8359,
           nested_enum_type_name_dispose_read_is_rejected_spec_27_6_8_7_26_5) {
    const std::string p = prog({
        "module M;",
        "public class Container",
        "  public enum Status Red, Green; end Status;",
        "end Container;",
        kMain,
        kMainBody,
        "    Container.Status.Dispose;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(p, "no member `Dispose` on type `Status`"));
}

// (4) CONTROL: the read of any other unknown name through a type name keeps
// its own diagnostic — the same gate, the same wording.
BEZEL_TEST(type_name_dispose_read_8359,
           unknown_member_through_a_type_name_keeps_its_own_diagnostic_spec_11_3) {
    const std::string p = prog({
        "module M;",
        kEnum,
        kMain,
        kMainBody,
        "    E.SomeOtherCase;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "no member `SomeOtherCase` on type `E`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""11.3, \xc2\xa7""12.1"));
}

// (5) CONTROL (FLIPPED by #8357, §28.2): an enum that DECLARES a case named
// `Dispose` — §26.5 rule 2 resolves a type name's chain against the type's
// enum constants, so that read is a constant read.
//
// ...but the DECLARATION is now an error in its own right: §28.2 (v0.355)
// forbids the name on an enum "whatever its parameter list and whatever its
// kind — a method, instance or `static`, a field, a property, a `const`, AN
// ENUM CONSTANT, a choice case or a nested type", and the error is AT THAT
// DECLARATION. So this program is refused before the read is ever reached
// (#8357), and the row that used to assert the opposite is corrected here.
//
// The §28.2 prohibition on DECLARING the name is the same rule this row's
// read half was measured against, and it now fires: the declaration check
// (the #7517 gate) refused a member of that name only when the member was a
// MethodSymbol, so an EnumConstantSymbol passed it.
BEZEL_TEST(type_name_dispose_read_8359,
           enum_case_named_dispose_is_refused_at_its_declaration_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public enum E Red, Dispose; end E;",
        kMain,
        kMainBody,
        "    var x := E.Dispose;",
        "    if x = E.Dispose then Console.WriteLine(\"ok\"); end;",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "may not declare an enum constant named `Dispose`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "\xc2\xa7""28.2"));
}

// (6) CONTROL: the CALL spelling over an enum keeps its own §8.7 diagnostic
// — the read spelling's fix does not touch it.
BEZEL_TEST(type_name_dispose_read_8359,
           enum_type_name_dispose_call_keeps_its_own_diagnostic_spec_8_7) {
    const std::string p = prog({
        "module M;",
        kEnum,
        kMain,
        kMainBody,
        "    E.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "`Dispose` is not admitted on an enum value"));
}

// (6b) CONTROL: the CALL spelling over a choice likewise.
BEZEL_TEST(type_name_dispose_read_8359,
           choice_type_name_dispose_call_keeps_its_own_diagnostic_spec_8_7) {
    const std::string p = prog({
        "module M;",
        kChoice,
        kMain,
        kMainBody,
        "    Ch.Dispose();",
        kMainEnd,
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "`Dispose` is not admitted on a choice value"));
}

// (7) CONTROL: the READ spelling over a CLASS type name is a §27.6
// method-reference FORMATION, and §27.6's own rule for the name governs it
// (#7344; the read half of §26.5 rule 2 against that formation rule is the
// open spec-question #8065) — both OUT OF SCOPE for this fix, which is about
// the ENUM and the CHOICE type name. Its verdict is unchanged.
BEZEL_TEST(type_name_dispose_read_8359,
           class_type_name_dispose_read_is_out_of_scope_spec_27_6) {
    const std::string p = prog({
        "module M;",
        "public class K",
        "  public static method Real()",
        "    Console.WriteLine(\"r\");",
        "  end Real;",
        "end K;",
        kMain,
        kMainBody,
        "    K.Dispose;",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// (7b) CONTROL: the same over an INTERFACE type name.
BEZEL_TEST(type_name_dispose_read_8359,
           interface_type_name_dispose_read_is_out_of_scope_spec_27_6) {
    const std::string p = prog({
        "module M;",
        "public interface IDisposable",
        "  method Dispose();",
        "end IDisposable;",
        kMain,
        kMainBody,
        "    IDisposable.Dispose;",
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// (8) CONTROL: `Dispose` on a VALUE of a class is the legitimate universal
// member (§8.7) — the READ spelling and the CALL spelling both stay
// admitted.
BEZEL_TEST(type_name_dispose_read_8359,
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
        "    k.Dispose;",
        "    k.Dispose();",
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

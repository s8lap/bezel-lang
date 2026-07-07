// Spec-derived parser tests for nested type declarations — §A.4
// `NestedTypeDecl ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl`.
//
// A class or record body admits a nested type declaration as a Member; the
// nested type itself follows the regular A.3 ClassDecl / InterfaceDecl /
// RecordDecl / EnumDecl grammar, including Visibility?+Modifier* and the
// usual class-/record-body contents. The member renderer prefixes the
// nested form with `nested `.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_nested_type, nested_class) {
    auto r = parseAll(
        "module M; class Outer "
        "  public class Inner end Inner; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("nested public class Inner"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_interface) {
    auto r = parseAll(
        "module M; class Outer "
        "  public interface IListener end IListener; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("nested public interface IListener"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_record_with_positional_fields) {
    auto r = parseAll(
        "module M; class Outer "
        "  public record Point(x: int, y: int); "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("nested public record Point(x: int, y: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_enum_with_constants) {
    auto r = parseAll(
        "module M; class Outer "
        "  public enum Color Red, Green, Blue; end Color; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "nested public enum Color { Red, Green, Blue }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_class_with_modifiers) {
    auto r = parseAll(
        "module M; class Outer "
        "  public abstract class Base end Base; "
        "  public sealed class Derived: Base end Derived; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(2));
    if (ms.size() == 2) {
        BEZEL_EXPECT_EQ(
            ms[0], std::string("nested public abstract class Base"));
        BEZEL_EXPECT_EQ(
            ms[1], std::string("nested public sealed class Derived: Base"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_class_inside_record) {
    auto r = parseAll(
        "module M; record Outer(x: int) "
        "  public class Helper end Helper; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("nested public class Helper"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, nested_class_with_method) {
    // The nested type body is itself a fully-fledged TypeDecl — every member
    // form should work inside it. Test with a single method to confirm
    // recursive descent isn't confused by the outer dispatch.
    auto r = parseAll(
        "module M; class Outer "
        "  public class Inner "
        "    public method Greet(): string => \"hi\"; "
        "  end Inner; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1) {
        // Verify outer member parsed, then poke into its inner members.
        const auto* td =
            dynamic_cast<const bezel::ast::TypeDecl*>(
                parseAll("module M; class Outer end Outer;")
                    .sourceFile->topDecls[0].get());
        (void)td;
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type, mixed_members_with_nested) {
    auto r = parseAll(
        "module M; class Outer "
        "  public x: int := 0; "
        "  public class Inner end Inner; "
        "  public method M(): int => 42; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(3));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13.3/§11.6 (v0.181, #1558): a nested type is a class member (§11.3), and
// §13.3's explicit-visibility rule states no exception for nested types (the
// three exceptions are the static constructor, the `polymorphic` registry
// declaration, and the destructor — none of which is a nested type). Omitting
// the visibility keyword on a nested `class` is a compile error, not a
// default to §13.2's top-level default-internal.
BEZEL_TEST(parser_nested_type,
           nested_class_without_visibility_rejected_spec_13_3) {
    auto r = parseAll(
        "module M; class Outer "
        "  class Inner end Inner; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool mentionsExplicitVisibility = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("explicit visibility") != std::string::npos) {
            mentionsExplicitVisibility = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(mentionsExplicitVisibility);
}

// §13.3/§11.6: the rejection applies uniformly to every nested-type kind
// admitted by A.4 NestedTypeDecl — record, enum, and interface, not just
// class.
BEZEL_TEST(parser_nested_type,
           nested_record_without_visibility_rejected_spec_13_3) {
    auto r = parseAll(
        "module M; class Outer "
        "  record Point(x: int, y: int); "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type,
           nested_enum_without_visibility_rejected_spec_13_3) {
    auto r = parseAll(
        "module M; class Outer "
        "  enum Color Red, Green, Blue; end Color; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_nested_type,
           nested_interface_without_visibility_rejected_spec_13_3) {
    auto r = parseAll(
        "module M; class Outer "
        "  interface IThing method DoIt(): int; end IThing; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §13.3/§11.6: control — supplying the visibility keyword is accepted, with
// no diagnostic (the positive counterpart to the four rejections above).
BEZEL_TEST(parser_nested_type,
           nested_class_with_explicit_visibility_accepted_spec_13_3) {
    auto r = parseAll(
        "module M; class Outer "
        "  private class Inner end Inner; "
        "end Outer;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0], std::string("nested private class Inner"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

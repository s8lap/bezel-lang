// Spec-derived parser tests for extension blocks (§26 / A.3
// `ExtensionDecl`) and extern blocks (§32 / A.3 `ExternBlock`).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// --- ExtensionDecl ---------------------------------------------------------

BEZEL_TEST(parser_ext_extern, empty_extension) {
    auto r = parseAll("module M; extension string end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("extension string"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extension_with_visibility) {
    auto r = parseAll("module M; public extension Foo end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("public extension Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extension_with_generic_target) {
    auto r = parseAll("module M; extension List<int> end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("extension List<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extension_with_member_field) {
    auto r = parseAll(
        "module M; extension Foo public x: int; end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("extension Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extension_with_constraints) {
    auto r = parseAll(
        "module M; extension T where T: IDisposable end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("extension T"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- ExternBlock -----------------------------------------------------------

BEZEL_TEST(parser_ext_extern, simple_extern_block) {
    auto r = parseAll(
        "module M; extern \"libc\" "
        "method puts(s: string): int; "
        "end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("extern \"libc\" { method puts(): int; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extern_block_with_convention) {
    auto r = parseAll(
        "module M; extern \"libfoo\" convention \"stdcall\" "
        "method foo(): int; "
        "end;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("extern \"libfoo\" convention \"stdcall\" "
                        "{ method foo(): int; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extern_block_multiple_decls) {
    auto r = parseAll(
        "module M; extern \"libc\" "
        "method strlen(s: string): int; "
        "method strcmp(a: string, b: string): int; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_ext_extern, extern_without_library_is_error) {
    auto r = parseAll("module M; extern method foo(); end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ext_extern, extern_with_no_methods_is_error) {
    // §A.3 ExternBlock requires ExternDecl+ (at least one).
    auto r = parseAll("module M; extern \"libc\" end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CG27f: stdlib §9.6 string methods — StartsWith, EndsWith, Contains,
// IndexOf, Substring. Implemented as compiler intrinsics using libc
// memcmp / memmem / malloc+memcpy under the hood.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §9.6: StartsWith(prefix): bool
BEZEL_TEST(codegen_cg27f, starts_with_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.StartsWith(\"Hello\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, starts_with_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.StartsWith(\"World\") else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_cg27f, starts_with_empty_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  return (1 if s.StartsWith(\"\") else 0); "
        "end Main; end C;"), 1);
}

// §9.6: EndsWith(suffix): bool
BEZEL_TEST(codegen_cg27f, ends_with_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.EndsWith(\"World\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, ends_with_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.EndsWith(\"Hello\") else 0); "
        "end Main; end C;"), 0);
}

// §9.6: Contains(sub): bool
BEZEL_TEST(codegen_cg27f, contains_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.Contains(\"lo W\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, contains_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.Contains(\"xyz\") else 0); "
        "end Main; end C;"), 0);
}

// §9.6: IndexOf(sub): int — first occurrence index, -1 if not found.
BEZEL_TEST(codegen_cg27f, index_of_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; return s.IndexOf(\"World\"); "
        "end Main; end C;"), 6);
}

BEZEL_TEST(codegen_cg27f, index_of_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; return s.IndexOf(\"xyz\"); "
        "end Main; end C;"), -1);
}

BEZEL_TEST(codegen_cg27f, index_of_start) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; return s.IndexOf(\"a\"); "
        "end Main; end C;"), 0);
}

// §9.6: Substring(start): string — from start to end.
BEZEL_TEST(codegen_cg27f, substring_from_offset) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(6); "
        "  return (1 if t = \"World\" else 0); "
        "end Main; end C;"), 1);
}

// §9.6: Substring(start, length): string — slice of given length.
BEZEL_TEST(codegen_cg27f, substring_with_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(0, 5); "
        "  return (1 if t = \"Hello\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, substring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(6); "
        "  return t.Length; "
        "end Main; end C;"), 5);
}

}  // namespace

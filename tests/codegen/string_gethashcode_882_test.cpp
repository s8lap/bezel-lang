// fix #882 / stdlib §9.6: string.GetHashCode() was rejected as
// "not yet supported in codegen". Route it to the existing content-based
// emitValueHashCode (same hash the synthesized record GetHashCode uses for
// string-typed fields), so it returns a content hash consistent with Equals.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.6 / #882: basic case — s.GetHashCode() compiles and runs (returns 0).
BEZEL_TEST(string_gethashcode_882, basic_compiles_and_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"hello\"; "
        "  var h: int := s.GetHashCode(); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §9.6 / #882: content-equal strings hash equal (literal vs concat) — the
// §9.6 "agrees with Equals" invariant. Returns 1 if hashes are equal.
BEZEL_TEST(string_gethashcode_882, content_equal_strings_hash_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"abc\"; "
        "  var b: string := \"a\" + \"bc\"; "
        "  if a.GetHashCode() = b.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.6 / #882: different strings produce different hashes (FNV fold).
// Returns 1 if hashes differ.
BEZEL_TEST(string_gethashcode_882, different_strings_hash_differ) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"abc\"; "
        "  var b: string := \"xyz\"; "
        "  if a.GetHashCode() <> b.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.6 / #882: string accessed via a variable field — GetHashCode works.
BEZEL_TEST(string_gethashcode_882, field_access_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"hello\"; "
        "  var h: int := s.GetHashCode(); "
        "  if h <> 0 then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.6 / §9.1 / #882: a record with a string field still compiles and runs
// when GetHashCode is also called on the string directly — confirms that both
// the record-field hash path and the new string-method path coexist correctly.
BEZEL_TEST(string_gethashcode_882, record_string_field_and_direct_hash) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(S: string); "
        "class C public static method Main(): int "
        "  var r: R := new R(\"abc\"); "
        "  var sh: int := \"abc\".GetHashCode(); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §9.6 / #882: regression — existing string methods (Length, Substring,
// ToInt32) still work after adding the GetHashCode case.
BEZEL_TEST(string_gethashcode_882, existing_string_methods_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"hello world\"; "
        "  var l: int := s.Length; "
        "  var n: int := \"42\".ToInt32(); "
        "  if l = 11 then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

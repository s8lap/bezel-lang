// Issue #4126 — the vtable-less built-in CLASSES that the `builtinClassSym`
// misdispatch reaches (`stdlib §6.1` / `§8.1` / `§2.6`), pinned by the
// RENDERED TEXT of the issue's own measured matrix.
//
// THE DEFECT (fixed by `narrow #4126`, commit 3a5a82ad4 — Stopwatch /
// CancellationToken / `Guid.GetHashCode` — and by its follow-up
// `narrow #4126`, commit 469e5eb9, which gave `Range<T>` its generated
// `Equals` / `GetHashCode`; both subjects name the issue in PROSE and so
// close nothing on GitHub).
// `SemaContext::builtinClassSym` stamps `kind = ast::TypeDeclKind::Class`
// UNCONDITIONALLY for every built-in it mints. The built-in classes that have
// NO dedicated `ToString` / `Equals` / `GetHashCode` interceptor in
// `Emitter::tryLowerBuiltinClassCall` therefore fell through to
// `lowerCallDispatch`'s generic universal-Object dispatch, which read the
// type's raw heap pointer (or, for `Guid`, its {i64,i64} VALUE) as a
// class-vtable object.
//
// MEASURED on the pre-fix tree (the parent of 3a5a82ad4), by the issue's own
// matrix:
//   Stopwatch.StartNew().ToString()    -> SIGSEGV (bz_string_to_utf8z)
//   Stopwatch.StartNew().GetHashCode() -> SIGSEGV (the same shape)
//   CancellationToken.None.ToString()  -> "null" (the null-receiver sentinel
//                                          for a representationally-null but
//                                          semantically non-null value)
//   a LIVE CancellationToken.ToString() -> SIGSEGV (the wild vtable jump)
//   CancellationToken.Equals()          -> tokens from DIFFERENT sources
//                                          compared Equal
//   CancellationToken.GetHashCode()     -> always 0
//   Guid.GetHashCode()                 -> always 0
//   Range<int>.Equals()                -> two structurally identical ranges
//                                          compared UNEQUAL, while `r1 = r2`
//                                          answered True
//   Range<int>.GetHashCode()            -> always 0
//
// The correct answers are the §8.5 / §8.3 defaults the type's own stdlib
// entry declares: `Stopwatch` (stdlib §8.1) and `CancellationToken`
// (stdlib §6.1) are CLASSES with no `ToString` override, so they render
// their qualified name; their `Equals` / `GetHashCode` are the §8.3
// reference-identity default. `Range<T>` (stdlib §9.8) is a body record, so
// it has §8.3's property-by-property VALUE equality and a content hash.
// `Guid` (stdlib §2.6) has a dedicated `Equals`, so equal Guids MUST hash
// identically.
//
// WHY THIS FILE IS NOT A COPY of `no_override_identity_builtins_4126_test.cpp`
// / `range_equals_gethashcode_4126_test.cpp` (the two `narrow #4126`
// commits' own rows): those files assert an EXIT CODE per cell. This file
// runs the issue's MATRIX as four programs and pins their exact stdout, so
// the oracle is the text a user sees.
//
// The oracle is a POSITIVE one: the exact stdout of a program that compiles
// and runs. There is no negative assertion here, so
// `scripts/check_vacuous_tests.py` has nothing to report.
//
// Cite: language spec §8.3 / §8.5 / §7.5 (v0.295, v0.338), stdlib §6.1
// (CancellationToken), §8.1 (Stopwatch), §2.6 (Guid), §9.8 (Range<T>).

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

std::string program(const std::string& imports, const std::string& body) {
    return "module M; " + imports + " class C public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

}  // namespace

// stdlib §8.1 `Stopwatch`: a CLASS with no `ToString` override, so it
// renders its qualified name — and its `Equals` is the §8.3 reference-
// identity default (two DISTINCT instances are unequal, self-equality holds),
// and its `GetHashCode` is the matching identity hash (stable per receiver).
// Pre-fix: `ToString()` and `GetHashCode()` SEGFAULTED the program.
BEZEL_TEST(no_override_identity_default_4126,
           stopwatch_object_contract_default_spec_8_1_stdlib_8_1) {
    auto r = runAot(program(
        "import System.Diagnostics; ",
        "  using var sw1 := Stopwatch.StartNew(); "
        "  using var sw2 := Stopwatch.StartNew(); "
        "  Console.WriteLine(sw1.ToString()); "
        "  Console.WriteLine(sw1.Equals(sw1)); "
        "  Console.WriteLine(sw1.Equals(sw2)); "
        "  Console.WriteLine(sw1.GetHashCode() = sw1.GetHashCode()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("System.Diagnostics.Stopwatch\nTrue\nFalse\nTrue\n"));
}

// stdlib §6.1 `CancellationToken`: a CLASS whose whole runtime
// representation is one borrowed pointer to the shared cancellation-state
// cell, so §8.3's reference identity IS content equality on that one field.
// Pre-fix: `.None.ToString()` rendered the null-receiver sentinel "null", a
// LIVE token's `ToString()` SEGFAULTED, distinct sources' tokens compared
// Equal, and `GetHashCode()` was always 0.
BEZEL_TEST(no_override_identity_default_4126,
           cancellation_token_object_contract_default_spec_6_1_stdlib_6_1) {
    auto r = runAot(program(
        "import System.Threading; ",
        "  var none := CancellationToken.None; "
        "  Console.WriteLine(none.ToString()); "
        "  Console.WriteLine(none.GetHashCode()); "
        "  using var cts1 := new CancellationTokenSource(); "
        "  using var cts2 := new CancellationTokenSource(); "
        "  var t1 := cts1.Token; "
        "  var t2 := cts2.Token; "
        "  Console.WriteLine(t1.ToString()); "
        "  Console.WriteLine(t1.Equals(t1)); "
        "  Console.WriteLine(t1.Equals(t2)); "
        "  Console.WriteLine(t1.GetHashCode() = t1.GetHashCode()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(
        r.stdoutText,
        std::string("System.Threading.CancellationToken\n0\n"
                    "System.Threading.CancellationToken\nTrue\nFalse\nTrue\n"));
}

// stdlib §9.8 `Range<T>`: a body record whose six properties are the type's
// auto-property surface, so §8.3 gives it property-by-property VALUE
// equality and a content hash. Pre-fix: two structurally identical ranges
// compared UNEQUAL (while `r1 = r2` answered True) and the hash was always 0.
BEZEL_TEST(no_override_identity_default_4126,
           range_value_semantics_default_spec_9_8_stdlib_9_8) {
    auto r = runAot(program(
        "",
        "  var r1 := 1..5; "
        "  var r2 := 1..5; "
        "  var r3 := 1..9; "
        "  Console.WriteLine(r1.Equals(r2)); "
        "  Console.WriteLine(r1.Equals(r3)); "
        "  Console.WriteLine(r1.GetHashCode() = r2.GetHashCode()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\n"));
}

// stdlib §2.6 `Guid`: it HAS a dedicated `Equals` (bytewise), so §8.3
// requires two EQUAL Guids to hash identically. Pre-fix `GetHashCode()` had
// no dedicated handler at all and answered 0 for every receiver.
BEZEL_TEST(no_override_identity_default_4126,
           guid_gethashcode_is_a_content_hash_spec_2_6_stdlib_2_6) {
    auto r = runAot(program(
        "import System; ",
        "  var g := Guid.NewGuid(); "
        "  var h := Guid.NewGuid(); "
        "  Console.WriteLine(g.GetHashCode() = h.GetHashCode()); "
        "  Console.WriteLine(g.GetHashCode() = g.GetHashCode()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

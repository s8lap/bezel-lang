// spec §32.11 (v0.333, "Symbol binding: names, aliases, and C-variadic
// imports") — the `:= symbol "name" variadic(N)?` clause of an extern method
// declaration (grammar A.3).
//
// This file owns the FRONT-END half of §32.11: the alias clause parses and
// resolves, `variadic(N)` is range-checked rather than crashing or wrapping,
// and the "No silent promotions" rule rejects a non-promoted type in a
// variadic position with a diagnostic naming the type to declare. The
// codegen half (the emitted signature and the call's `%al`) is owned by
// tests/codegen/extern_variadic_codegen_32_11_test.cpp.
//
// The four `s32_11_*` accept rows in tests/conformance/rules/manifest.tsv
// are `lib`-shaped: they compile with `-c` and never link, so nothing in the
// corpus could see a symbol name emitted with its surrounding quotes. These
// rows therefore assert the DIAGNOSTIC, never the exit status — per the lane
// brief, a `lib` case with no `Main` rejects with "program has no entry
// point" on every tree forever, so an rc-only comparison would manufacture
// false agreement.
//
// A rule needs BOTH an admit-row and a reject-row: "rejects everything"
// passes a suite of only reject-rows. `float_outside_variadic_position_
// stays_silent` is the admit row for the promotion rule, differing from the
// `float` reject row in exactly one respect — the `variadic(N)` clause.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §32.11's reject diagnostics all name the promotion rule's own heading and
// the type to declare. Pinning on the two together keeps a §22.x or §32.5
// diagnostic from satisfying a §32.11 row by accident.
bool hasNoSilentPromotionsDiag(const JitResult& r, const std::string& ty,
                               const std::string& replacement) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§32.11") == std::string::npos) continue;
        if (d.message.find("No silent promotions") == std::string::npos) continue;
        if (d.message.find("variadic position") == std::string::npos) continue;
        if (d.message.find(ty) == std::string::npos) continue;
        if (!replacement.empty() &&
            d.message.find(replacement) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// A `variadic(N)` arity that cannot be represented / is out of range. Asserted
// on the message: the pre-fix parser ran `std::stoull` on an unchecked
// literal, which threw `std::out_of_range` out of an uncaught path.
bool hasVariadicArityRangeDiag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("variadic arity") == std::string::npos) continue;
        if (d.message.find("out of range") == std::string::npos &&
            d.message.find("exceeds maximum representable") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// A whole program whose `Main` prints a fixed marker, so an accept row is
// distinguishable from a silently-degraded one (the #2434 backstop lowers an
// unlowerable call to zero, which still exits 0).
JitResult runProgram(const std::string& src) {
    return runAot(src +
                  "public class Program "
                  "  public static method Main() "
                  "    Console.WriteLine(4242); "
                  "  end Main; "
                  "end Program;");
}

}  // namespace

// ============================================================================
// ACCEPT ROWS — the clause parses, resolves, and links.
// ============================================================================

// §32.11 row s32_11_0001: "The alias clause `:= symbol "name"`, written after
// the return clause, binds the declaration to the named symbol instead."
// (manifest row 1775, UNBANKED at the time of writing.)
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           symbol_alias_clause_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method GetVersion(): int := symbol \"example_get_version\"; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4242") != std::string::npos);
}

// §32.11: "`N` equal to the arity is legal and load-bearing ... the only sound
// minimum-arity binding of a variadic symbol." Manifest row s32_11_0003.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_n_equal_to_arity_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libc\" "
        "  method Open2(path: CString, flags: int): int "
        "      := symbol \"open\" variadic(2); "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4242") != std::string::npos);
}

// §32.11 row s32_11_0004: "variadic(N) with N less than the declared arity
// passes the trailing parameters variadically."
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_n_less_than_arity_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libc\" "
        "  method OpenCreate(path: CString, flags: int, mode: uint): int "
        "      := symbol \"open\" variadic(2); "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4242") != std::string::npos);
}

// §32.11: "`N` ≤ the declared arity" — N == 0 is the lower bound, a symbol
// called with every declared parameter in a variadic slot.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_zero_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libc\" "
        "  method Open2(path: CString, flags: int): int "
        "      := symbol \"open\" variadic(0); "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4242") != std::string::npos);
}

// The promoted-type list of §32.11 "No silent promotions", row by row: int,
// uint, long, ulong, double, IntPtr, UIntPtr, CString, WString, Ptr<T>. This
// is the ADMIT half of the promotion rule — a suite of only reject-rows would
// also pass if the predicate simply rejected every variadic declaration.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           promoted_types_in_variadic_position_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "import System.Interop; "
        "extern \"libexample\" "
        "  method M(a: int, b: uint, c: long, d: ulong, e: double, "
        "           f: IntPtr, g: UIntPtr, h: CString, i: WString, "
        "           j: Ptr<byte>) "
        "      := symbol \"example_m\" variadic(0); "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// ============================================================================
// ADMIT ROW — the promotion rule must stay SILENT outside a variadic
// position. Differs from the `float` reject row in exactly one respect: the
// `variadic(N)` clause is absent, so `value` is a fixed parameter and C's
// default-argument promotions do not apply to it.
// ============================================================================

BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           float_outside_variadic_position_stays_silent_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr, value: float): int := symbol \"example_m\"; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4242") != std::string::npos);
}

// Same control for `bool`, whose promotion text differs from `float`'s.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           bool_outside_variadic_position_stays_silent_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr, value: bool): int := symbol \"example_m\"; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// ============================================================================
// REJECT ROWS — §32.11 "No silent promotions" and the arity range check.
// ============================================================================

// Manifest row s32_11_0005: "Declaring `float` ... in a variadic position is
// a compile error naming the type to write ("C promotes `float` variadic
// arguments to `double`; declare the parameter as `double`")." The
// diagnostic must NAME `double`.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           float_in_variadic_position_rejected_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method SetOptFloat(h: IntPtr, opt: int, value: float): int "
        "      := symbol \"example_setopt\" variadic(2); "
        "end; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasNoSilentPromotionsDiag(r, "float", "double"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §32.11: "`RawBytes` is excluded from variadic positions: its two-slot
// marshalling (§32.5) does not fit the one-slot-per-parameter promotion
// accounting the `%al`/stack contract depends on — pass `Ptr<byte>`/`IntPtr`
// plus an explicit length." The diagnostic must name that replacement. Prior
// to the rule being enforced at all this compiled clean.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           rawbytes_in_variadic_position_rejected_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr, b: RawBytes): int "
        "      := symbol \"example_m\" variadic(1); "
        "end; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNoSilentPromotionsDiag(
        r, "RawBytes", "explicit length"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A `RawBytes` parameter at a FIXED position of a variadic declaration is
// NOT covered by §32.11's exclusion — its two-slot marshalling is §32.5-legal
// there. Only the trailing parameters (index ≥ N) are in scope of the rule.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           rawbytes_in_fixed_position_of_variadic_decl_accepted_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(b: RawBytes, flag: int): int "
        "      := symbol \"example_m\" variadic(1); "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// `RawBytes` as a FIXED parameter of an ordinary fixed-arity extern (no
// `variadic` clause at all) must stay admitted — the ADMIT control for the
// reject row above, differing in exactly the clause.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           rawbytes_outside_variadic_position_stays_silent_spec_32_11) {
    auto r = runProgram(
        "module Conf; "
        "extern \"libc\" "
        "  method WriteAll(fd: int, b: RawBytes): int; "
        "end; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// `RawBytes` as a FIXED parameter (index 0 < variadic(1)) must stay admitted —
// §32.5's two-slot marshalling applies there and §32.11's exclusion does not.

// The remaining non-promoted scalars of §32.11's list, each named with its own
// replacement text.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           narrow_int_types_in_variadic_position_rejected_spec_32_11) {
    const char* kRows[] = {
        "method A(h: IntPtr, v: bool): int := symbol \"m\" variadic(1);",
        "method B(h: IntPtr, v: byte): int := symbol \"m\" variadic(1);",
        "method C(h: IntPtr, v: sbyte): int := symbol \"m\" variadic(1);",
        "method D(h: IntPtr, v: short): int := symbol \"m\" variadic(1);",
        "method E(h: IntPtr, v: ushort): int := symbol \"m\" variadic(1);",
    };
    for (const char* decl : kRows) {
        auto r = runProgram(std::string("module Conf; "
                                        "extern \"libexample\" ") +
                           decl + " end; ");
        BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
        BEZEL_EXPECT_TRUE(
            hasNoSilentPromotionsDiag(r, "variadic position", ""));
        BEZEL_EXPECT_FALSE(r.ran);
    }
}

// A `variadic(N)` literal larger than the representable range. The pre-fix
// parser called `std::stoull` on the unchecked literal and let
// `std::out_of_range` escape — an uncaught exception, i.e. a compiler CRASH,
// not a diagnostic. Assert on the diagnostic, never on an exit status: this is
// a `lib`-shaped program, so its exit code is uninformative by construction.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_arity_overflow_is_a_diagnostic_not_a_crash_spec_32_11) {
    auto r = runAot(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr): int "
        "      := symbol \"example_m\" variadic(99999999999999999999999999999); "
        "end; "
        "public class Program "
        "  public static method Main() Console.WriteLine(4242); end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVariadicArityRangeDiag(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// UINT32_MAX + 1. The pre-fix parser stored this into a uint32_t and it
// wrapped to 0, turning the declaration into `variadic(0)` — a silent
// acceptance of a value the source never spelled.
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_arity_wraparound_rejected_spec_32_11) {
    auto r = runAot(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr): int "
        "      := symbol \"example_m\" variadic(4294967296); "
        "end; "
        "public class Program "
        "  public static method Main() Console.WriteLine(4242); end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVariadicArityRangeDiag(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §32.11: "`N` ≤ the declared arity". A `variadic(N)` naming MORE fixed
// parameters than the declaration has is a spec violation and must be
// rejected, not silently treated as "every parameter is variadic".
BEZEL_TEST(extern_symbol_alias_variadic_32_11,
           variadic_n_greater_than_arity_rejected_spec_32_11) {
    auto r = runAot(
        "module Conf; "
        "extern \"libexample\" "
        "  method M(h: IntPtr): int := symbol \"example_m\" variadic(3); "
        "end; "
        "public class Program "
        "  public static method Main() Console.WriteLine(4242); end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}
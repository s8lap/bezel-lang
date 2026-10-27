// Spec-derived regression test for issue #7319 — the ONE-ARGUMENT `ToString`
// member call (`ToString(format: string)` and `ToString(radix: int)`) on a
// §7.4-NARROWED NULLABLE PRIMITIVE receiver formatted the receiver's
// CARRIER, not its PAYLOAD (language spec §7.4 / §7.11 / §22.6; stdlib
// §2.18 / §9.1, v0.344).
//
// THE DEFECT. A nullable value type lowers to the tagged `{i8 hasValue; T
// value}` struct (`isNullableValueTy`, the one predicate that names that
// representation). The NO-ARGUMENT member-call arm asked whether the receiver
// was NULLABLE and, when it was, delegated to `emitValueToString` — whose FIRST
// arm branches on the tag and renders the PAYLOAD (§8.5's shared authority).
// The ONE-ARGUMENT arm never asked: it handed the whole struct to the
// formatter, so
//
//   * an `int?` printed the carrier's BYTES — a plausible-looking number that
//     no exit-code oracle can distinguish from the right one;
//   * a `double?` reached an `fcmp` against a `double` — MALFORMED IR, and the
//     compiler ABORTED ("Invalid operand types for FCmp instruction");
//   * a `bool?`/`char?`/`byte?`/`short?` reached a `trunc`/`zext`/`sext` or a
//     runtime call typed for the bare payload — also malformed IR.
//
// MEASURED ON THE BASE ARM (this lane's tree at 89055374), one statement per
// compile, `x` narrowed by `if x <> null then`:
//
//   | receiver                | base arm                                | §2.18 answer |
//   |-------------------------|-----------------------------------------|--------------|
//   | `int?    := 5`  "D5"   | IR-verifier ABORT (fptosi on a struct)  | `00005`      |
//   | `int?    := 5`  "F4"   | the carrier's bytes (e.g. `73729`)       | `5`          |
//   | `double? := 1.5` "F4"  | IR-verifier ABORT (fcmp on a struct)     | `1.5000`     |
//   | `float?  := 1.25f` "F4"| IR-verifier ABORT (fpext on a struct)    | `1.2500`     |
//   | `decimal?:= 2.5m` "F2" | the carrier's bytes (a garbage decimal; the  | `2.50`       |
//   |                         | value is NOT stable across builds - see   |              |
//   |                         | the note at the `decimal` row below)      |              |
//   | `bool?   := true`      | IR-verifier ABORT (trunc on a struct)   | compile error|
//   | `char?   := 'Z'`       | IR-verifier ABORT (a struct argument)   | compile error|
//   | `byte?   := 200`       | IR-verifier ABORT (zext on a struct)    | `200`        |
//   | `short?  := 300`       | IR-verifier ABORT (sext on a struct)    | `300`        |
//   | `int?    := 255` `(16)`| IR-verifier ABORT (sext on a struct)    | `ff`         |
//   | `long?   := 255` `(16)`| IR-verifier ABORT (sext on a struct)    | `ff`         |
//   | `ulong?  := 255` `(16)`| IR-verifier ABORT (sext on a struct)    | `ff`         |
//
// ⚠ THE "CARRIER'S BYTES" ROWS ARE THE DANGEROUS ONES — the program COMPILES
// and exits 0. An exit-code oracle, or a "did it compile" oracle, passes them
// VACUOUSLY. Every row below asserts the program's EXACT stdout, and every
// payload is chosen so that no reading of the carrier can produce it: the
// carrier's `hasValue` tag is `1` and its payload slot holds the value, so the
// rendered text of a carrier is a number the value's own rendering can never
// equal.
//
// THE GOVERNING TEXT, QUOTED.
//
// Language spec §7.4 (v0.344): "Member access, method calls, and indexing on
// a nullable value are compile errors unless the compiler has narrowed it to
// non-null". The narrowing is established by §7.4's own list, which includes
// the null test these programs use ("a null test in either spelling — `x <>
// null` or `x is not null` … in an `if`/`elif` condition") and the
// declaration form ("from a declaration whose initializer is such an `expr` —
// `var x: T? := expr;` … which is the declaration's assignment").
//
// Language spec §22.6 (v0.344): "a read of a binding §7.4 has narrowed
// non-null is of its non-nullable type" — so AT THIS CALL SITE the receiver
// IS the `int` / `double` / `decimal` it denotes.
//
// Standard library §2.18 (v0.344) fixes the format overload's VOCABULARY — the
// integer row is "`D`/`Dn` decimal (n = min digits, zero-padded) · `X`/`x` hex
// (upper/lower, `Xn` pads) · `B` binary", the floating row's is "`F`/`Fn`
// fixed-point (n decimals, default 2) · `E`/`En` scientific · `G`/`Gn` general
// · `P`/`Pn` percent (×100, `%` suffix) · `R` round-trip", and the `decimal`
// row's is "`F`/`Fn`, `E`/`En`, `G`, `P`/`Pn`". Standard library §2.3 (v0.344)
// states the rendering rule the one-argument `ToString` follows for the
// `Convert` family — "Each one-argument `ToString` returns the text the value
// it receives renders as under its own no-format `ToString()`" — and standard
// library §9.1 (v0.344) fixes the radix overload: "`ToString(radix)` emits the
// lower-case digits … and takes the same radix set".
//
// WHY THE UN-NARROWED FORM IS A ROW AND NOT A FIX. §7.4 forbids the member
// access outright, and sema enforces it: an un-narrowed nullable receiver is
// REJECTED with "member access `.ToString` on a nullable value requires
// narrowing it to non-null first (§7.4)". The compiler therefore never sees
// an un-narrowed nullable receiver at this site, which is exactly the
// precondition `unwrapNarrowedNullableOperand` names ("Extracting the payload
// is therefore correct ONLY at a site where a SEMA GATE already rejects the
// un-narrowed form"). The row below PINS that rejection, so the codegen
// unwrap can never be read as "the compiler silently accepts an un-narrowed
// nullable receiver".
//
// THE CONTROLS. The no-argument arm, the §18.5 interpolation hole with a
// specifier, the null-forgiving `!` spelling, the non-nullable receiver, and
// the `DateTime?` member of the Named/value-builtin family are all green on
// BOTH arms — they are what proves the fix did not over-reach into a sibling
// spelling, and (for the `DateTime?` row) that the family whose member call
// has its OWN arm was never affected.
//
// THE RESIDUALS THIS FILE DOES NOT PIN (each is a SEPARATE, RECEIVER-
// INDEPENDENT defect, reported by this lane, and each fires IDENTICALLY for a
// NON-nullable receiver):
//
//   * the literal `D`/`X` format on an integer receiver that is NOT 32 bits
//     wide (`byte`, `sbyte`, `short`, `ushort`, `long`, `ulong`) emits
//     malformed IR — `fptosi i8`/`i16`/`i64` to `i32` — for the NON-nullable
//     spelling too (`var v: byte := 200; v.ToString("D5")` aborts). The rows
//     below therefore use the RADIX overload for the narrow and wide integer
//     receivers, and the `D`/`X` literal overload for the 32-bit ones;
//   * stdlib §2.18's table gives `bool` and `char` NO specifier ("a format
//     spec on these is a compile error"), and the compiler compiled
//     `bool.ToString("F4")` anyway — for the NON-nullable spelling too.
//     CLOSED BY #7333: the `bool?`/`char?` rows below now assert the §2.18
//     compile error, and the NULLABILITY property they used to pin (the
//     payload is rendered, not the carrier) is still pinned by the numeric
//     rows, whose receivers DO admit a specifier;
//   * the literal `D` format on an UNSIGNED integer payload renders it through
//     a SIGNED conversion (`var v: uint := 4000000000; v.ToString("D5")`
//     prints "-294967296") — for the NON-nullable spelling too, so the
//     unsigned rows below use the hex specifier, which renders the BITS.
//
// THE ORACLE. `runAotChecked` — the generated-code AddressSanitizer +
// LeakSanitizer net at exit — so the string the fix routes through the
// formatter is also checked for a leak and a use-after-free. `runAot`'s
// `r.ran` is false for FOUR outcomes, not one (a reject, a compiler crash, a
// runtime signal, a timeout), so the row that must detect "the compiler did
// NOT emit malformed IR" asserts the COMPILE'S SUCCESS (`hasErrors()`) AND
// the program's EXACT stdout together — a program that printed nothing and a
// program that never compiled are then different failures.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// A program whose `Main` body is `body`; every row below is one of these.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The narrowed shape: a local of a nullable primitive type, narrowed by
// §7.4's null test, and the one-argument `ToString` inside the guarded block.
// `decl` is the declaration, `call` the member call.
std::string narrowed(const std::string& decl, const std::string& call) {
    return "    " + decl + "\n"
           "    if x <> null then\n"
           "      Console.WriteLine(" + call + ");\n"
           "    end if;\n";
}

// A declaration-narrowed shape: §7.4's "assignment: … likewise after a
// declaration whose initializer is such an `expr` — `var x: T? := expr;`" —
// so the call needs no guard.
std::string declared(const std::string& decl, const std::string& call) {
    return "    " + decl + "\n"
           "    Console.WriteLine(" + call + ");\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PAYLOAD ROWS — the one-argument `ToString(format)` overload.
// ---------------------------------------------------------------------------

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — a §7.4-narrowed `int?` renders
// the VALUE. "D5" is §2.18's integer vocabulary ("`D`/`Dn` decimal (n = min
// digits, zero-padded)"), so 5 renders "00005". On the base arm this row
// ABORTED the compiler (a `fptosi` of the `{i8, i32}` carrier).
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_int_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: int? := 5;", "x.ToString(\"D5\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — the DOUBLE receiver is the row
// that carried the COMPILER ABORT ("Invalid operand types for FCmp
// instruction": the `{i8, double}` carrier reached an `fcmp` against a
// `double`). "F4" is §2.18's floating vocabulary ("`F`/`Fn` fixed-point (n
// decimals, default 2)"), so 1.5 renders "1.5000".
//
// THE ORACLE FOR "THE COMPILER DID NOT EMIT MALFORMED IR" IS THE COMPILE'S
// SUCCESS AND THE PROGRAM'S OUTPUT, ASSERTED TOGETHER: `hasErrors()` is false
// (the compiler accepted the program and produced an executable) AND the
// program ran AND it printed exactly "1.5000". A program that compiled but
// printed nothing, and a program that never compiled, are then two different
// failures — which a stdout-only assertion could not tell apart.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_double_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: double? := 1.5;", "x.ToString(\"F4\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.5000\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — the `float` receiver (its own
// LLVM type, so the base arm's failure was an `fpext` of the `{i8, float}`
// carrier). "F4" renders 1.25 as "1.2500".
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_float_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: float? := 1.25f;", "x.ToString(\"F4\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.2500\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — the `decimal` receiver, whose
// payload is a 16-byte struct (not a scalar), so the base arm's failure was a
// SILENT one in a plain build: the carrier's bytes were formatted as the value
// ("2.50" became a garbage decimal). "F2" is §2.18's decimal vocabulary, so
// 2.5m renders "2.50".
//
// THE BASE ARM'S GARBLED VALUE IS NOT A STABLE NUMBER: two separate base-arm
// builds printed two different decimals, and under `--sanitize=address` the
// same program is an ASan STACK-BUFFER-OVERFLOW ("WRITE of size 20"). The
// FIXED arm prints "2.50" deterministically, and this row fails on the base
// arm and passes on the fixed arm on both oracles.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_decimal_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: decimal? := 2.5m;", "x.ToString(\"F2\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2.50\n");
}

// #7333 / stdlib §2.18 (v0.344) — the `bool` receiver, at the NARROWED
// spelling. §2.18's table gives `bool` NO specifier ("a format spec on these
// is a compile error"), so `x.ToString("F4")` on a `bool` is a COMPILE ERROR —
// at the non-nullable spelling and at this narrowed one alike (spec §22.6: at
// this call site the receiver IS the `bool` it denotes).
//
// ⚠ THIS ROW FLIPPED WITH #7333. It pinned #7319's NULLABILITY property (the
// payload is rendered, not the carrier) on a receiver whose specifier §2.18
// does not admit; the property itself is still pinned by the numeric rows
// above, whose receivers DO admit a specifier. The verdict is now the compile
// error §2.18 requires — the oracle is a diagnostic at a REAL location, not
// an exit code.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_bool_receiver_with_a_format_spec_is_rejected_2_18) {
    JitResult r = runAot(
        prog(narrowed("var x: bool? := true;", "x.ToString(\"F4\")")));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "a format spec requires an intrinsically-formattable receiver"));
}

// #7333 / stdlib §2.18 (v0.344) — the `char` receiver, at the NARROWED
// spelling. §2.18's table gives `char` NO specifier, exactly as it gives
// `bool` none ("a format spec on these is a compile error"), so
// `x.ToString("F4")` on a `char` is a COMPILE ERROR.
//
// ⚠ THIS ROW FLIPPED WITH #7333, for the same reason as the `bool` row above.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_char_receiver_with_a_format_spec_is_rejected_2_18) {
    JitResult r = runAot(
        prog(narrowed("var x: char? := 'Z';", "x.ToString(\"F4\")")));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "a format spec requires an intrinsically-formattable receiver"));
}

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — the `uint` receiver. The hex
// specifier renders the payload's BITS, so it is the spelling that shows the
// unsigned value unchanged: §2.18's integer row gives "`X`/`x` hex (upper/
// lower, `Xn` pads)", and "X4" pads to four digits, so 255 renders "00FF".
//
// (A `D` specifier on an unsigned payload would render it through a SIGNED
// conversion — `var x: uint := 4000000000; x.ToString("D5")` prints
// "-294967296" — a separate, receiver-independent gap that this lane reports;
// the hex conversion has no signedness arm, so the row below is the faithful
// spelling for this receiver.)
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_uint_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: uint? := 255;", "x.ToString(\"X4\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00FF\n");
}

// ---------------------------------------------------------------------------
// THE PAYLOAD ROWS — the one-argument `ToString(radix)` overload.
// ---------------------------------------------------------------------------

// #7319 / spec §7.4 + §22.6 + stdlib §9.1 — the radix overload of the same
// one-argument family. §9.1: "`ToString(radix)` emits the lower-case digits".
// 255 in base 16 is "ff".
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_int_receiver_renders_the_radix_9_1) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: int? := 255;", "x.ToString(16)")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §9.1 — the `long` receiver, whose
// payload is 64 bits wide. §9.1 fixes the radix rendering for EVERY integer
// type ("`T.Parse(n.ToString(radix), radix)` is n for every value of every
// integer type and every admitted radix"), so 255 in base 16 is "ff" here too
// — and NOT a 32-bit truncation of the value.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_long_receiver_renders_the_radix_9_1) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: long? := 255;", "x.ToString(16)")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §9.1 — the `ulong` receiver. §9.1's
// radix alphabet is the lower-case digits for the admitted radices, so 255 in
// base 16 is "ff" — the unsigned value, never a sign-extended one.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_ulong_receiver_renders_the_radix_9_1) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: ulong? := 255;", "x.ToString(16)")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §9.1 — the `byte` receiver, whose
// payload is 8 bits wide. 200 in base 16 is "c8" — the value, never the
// carrier's tag-and-slot bytes.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_byte_receiver_renders_the_radix_9_1) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: byte? := 200;", "x.ToString(16)")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "c8\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §9.1 — the `short` receiver, whose
// payload is 16 bits wide. 300 in base 16 is "12c".
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_short_receiver_renders_the_radix_9_1) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: short? := 300;", "x.ToString(16)")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "12c\n");
}

// ---------------------------------------------------------------------------
// THE OTHER ROUTES TO THE SAME RECEIVER.
// ---------------------------------------------------------------------------

// #7319 / spec §7.4 + §22.6 — the DECLARATION narrowing (§7.4's "from a
// declaration whose initializer is such an `expr` — `var x: T? := expr;` …
// which is the declaration's assignment") rather than the null test. The
// receiver is read at the call with the same carrier, so the same unwrap
// applies.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_declared_nullable_int_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        prog(declared("var x: int? := 5;", "x.ToString(\"D5\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / spec §7.4 + §22.6 — a PARAMETER receiver (§7.4's "parameters other
// than `ref` parameters" are narrowing carriers). The parameter is a local of
// the method, so its read at the call loads the same carrier.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_parameter_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Take(n: int?): string\n"
        "    if n <> null then\n"
        "      return n.ToString(\"D5\");\n"
        "    end if;\n"
        "    return \"\";\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Take(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / spec §7.4 + §22.6 — a `readonly` FIELD receiver. §7.4's narrowing
// carriers are "local variables, parameters other than `ref` parameters, and
// `readonly` fields", so a narrowed read of a readonly field is a legal
// member call; the field's read loads the same carrier.
//
// ⚠ A MUTABLE field is NOT a narrowing carrier, so the same call on a mutable
// field is a compile error (§7.4) — pinned by the row below.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_readonly_field_receiver_formats_the_value_7_4) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class Holder\n"
        "  public readonly F: int?;\n"
        "  public constructor(v: int) this.F := v; end constructor;\n"
        "end Holder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var h := new Holder(5);\n"
        "    if h.F <> null then\n"
        "      Console.WriteLine(h.F.ToString(\"D5\"));\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / spec §7.4 + §22.6 + stdlib §2.18 — the RUNTIME format spelling
// (stdlib §9.1's `ToString(format: string)` with a format held in a variable
// rather than a literal). It is the same overload and the same receiver, so
// it must render the payload too.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_receiver_with_a_runtime_format_formats_the_value_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x: int? := 5;\n"
        "    var f: string := \"D5\";\n"
        "    if x <> null then\n"
        "      Console.WriteLine(x.ToString(f));\n"
        "    end if;\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// ---------------------------------------------------------------------------
// THE UN-NARROWED FORM — §7.4's REJECTION, PINNED.
// ---------------------------------------------------------------------------

// #7319 / spec §7.4 — the member access on an UN-narrowed nullable receiver
// is a compile error, and the compiler says so. This row is GREEN ON BOTH
// ARMS: it pins the behaviour the codegen unwrap DEPENDS ON (the unwrapper's
// own precondition is "a SEMA GATE already rejects the un-narrowed form"), so
// a future change that let the un-narrowed form through would turn this row
// RED and stop the unwrap from being sound.
//
// The receiver is a PARAMETER, so §7.4's declaration narrowing cannot have
// established anything for it: a parameter is narrowed only by the forms §7.4
// lists, and none of them applies at the method's entry.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_un_narrowed_nullable_receiver_is_rejected_7_4) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Take(n: int?): string\n"
        "    return n.ToString(\"D5\");\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Take(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "requires narrowing it to non-null"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS — green on BOTH arms.
// ---------------------------------------------------------------------------

// #7319 / spec §8.5 — the NO-ARGUMENT arm, the spelling that was already
// right: it delegates a nullable receiver to `emitValueToString`, which
// branches on the `{i8 hasValue; T}` tag and renders the payload. This row is
// what proves the fix did not over-reach into the sibling spelling, and it is
// the contrast the issue itself rests on.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_no_argument_narrowed_nullable_receiver_is_unchanged_8_5) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: int? := 5;", "x.ToString()")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// #7319 / spec §18.5 — the interpolation hole WITH a format specifier, the
// other formatted surface §2.18 names ("String interpolation dispatches a
// hole's format spec here"). The hole renders the payload already — its own
// path reads the value out of the carrier — so this row is green on BOTH arms
// and proves the fix did not disturb the hole path.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_interpolation_hole_with_a_specifier_is_unchanged_18_5) {
    JitResult r = runAotChecked(prog(
        "    var x: int? := 5;\n"
        "    if x <> null then\n"
        "      Console.WriteLine($\"{x:D5}\");\n"
        "    end if;\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / spec §7.8 — the null-forgiving spelling `x!`, whose result is "the
// non-nullable form of the expression". Green on BOTH arms.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_null_forgiving_receiver_formats_the_value_7_8) {
    JitResult r = runAotChecked(
        prog(declared("var x: int? := 5;", "x!.ToString(\"D5\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / stdlib §2.18 — the NON-nullable receiver, which is the ANSWER the
// nullable receiver must now agree with. Green on BOTH arms: it says what
// "5" renders as, so a nullable row that printed anything else was wrong.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_non_nullable_int_receiver_renders_the_same_text_2_18) {
    JitResult r = runAotChecked(
        prog(declared("var x: int := 5;", "x.ToString(\"D5\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// #7319 / stdlib §2.18 — the `DateTime?` receiver, the member of the
// Named/value-builtin family whose one-argument `ToString(format)` has its
// OWN arm (and is NOT the primitive arm this fix changed). Green on BOTH
// arms: it pins that the family was never affected, so the fix's scope is
// the primitive arm and nothing else.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_narrowed_nullable_datetime_receiver_is_unchanged_2_18) {
    JitResult r = runAotChecked(
        prog(narrowed("var x: DateTime? := new DateTime(2024, 3, 15);",
                      "x.ToString(\"yyyy-MM-dd\")")));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2024-03-15\n");
}

// #7319 / spec §7.4 — the MUTABLE field is not a narrowing carrier, so the
// same member access on it is a compile error. Green on BOTH arms: it pins
// the §7.4 rule that lets the codegen unwrap be sound for the field case
// (the `readonly` row above) — a mutable field's read is NOT narrowed, so the
// compiler must never admit the call.
BEZEL_TEST(nullable_receiver_tostring_format_7319,
           the_un_narrowed_nullable_mutable_field_receiver_is_rejected_7_4) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class Holder\n"
        "  public F: int?;\n"
        "  public constructor(v: int) this.F := v; end constructor;\n"
        "end Holder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var h := new Holder(5);\n"
        "    if h.F <> null then\n"
        "      Console.WriteLine(h.F.ToString(\"D5\"));\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "requires narrowing"));
}

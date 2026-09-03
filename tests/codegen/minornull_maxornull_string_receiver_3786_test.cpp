// Codegen + Sema — stdlib §5.1 / §9.6 (v0.286), issue 3786: `"s".MinOrNull()`
// / `.MaxOrNull()` were rejected by the sema-level string-receiver LINQ
// exemption gate (`common::isStringLinqOperatorMemberName`,
// `src/common/BuiltinRegistry.cpp`'s `kStringLinqOperatorMemberNames`, then
// 37 names) with "string member `.MinOrNull` is not yet supported in
// codegen", even though the sibling table for the SAME question on the
// builtin-collection family (`kBuiltinCollectionLinqOperatorNames`, 38
// names) already admitted both. This is the residual half of issue #3444,
// which explicitly declined to fix it by "adding the four names to the
// list" without first proving the string-receiver materialize path lowers
// them correctly end-to-end — because a wrongly-lowered admitted name
// converts a loud reject into a silent wrong value, the failure direction
// this project never accepts.
//
// THE PROOF (done before this table changed, not assumed). With fix #3764
// (materialize MinOrNull/MaxOrNull like every other §5 terminal on every
// non-array source family — a SEPARATE, cherry-picked fix this lane does not
// own) in place, `isStringRecvTerm` already sits in the SAME `lazyTerminal`
// disjunction as `isArrTerminal`, so once `MinOrNull`/`MaxOrNull` joined
// `isArrTerminal`, a `string` receiver materializes via
// `bz_str_to_char_array` -> `bz_lazy_to_array(lazy, esz=4)` and runs the
// EXISTING #2783 array arm — exactly the same route First/Sum/Min/Max/...
// already take for a string receiver. Measured on this tree BEFORE adding
// the two names (by editing the table directly and rebuilding, then
// checking the value, not just the exit code):
//   `"banana".MinOrNull()` -> 97 ('a'), `.MaxOrNull()` -> 110 ('n') CORRECT
//   `"".MinOrNull()` / `.MaxOrNull()` -> BOTH compared `= null` as `False`
//     (should be `True`) — the array arm's `elemTypeRef` derivation
//     (`recvTy->genericArgs[0]`) is `nullptr` for a `string` receiver
//     (`recvTy` is the bare `string` Primitive TypeRef, no genericArgs), so
//     the `isValTy` decision that picks between the `{i8 hasValue; T value}`
//     tagged-struct encoding and the "null pointer = null" encoding wrongly
//     took the LATTER for `char` (a value type, §4.5, fix #849) and used a
//     bare i32 zero as the sentinel — indistinguishable from the valid code
//     point U+0000 and, worse, a completely different LLVM representation
//     than what `char?` sema-types as and what a downstream `!`/`??`
//     consumer expects.
//
// THE FIX (in `LlvmBackend_Calls.cpp`'s MinOrNull/MaxOrNull arm, owned by
// this lane): when `isStringRecvTerm`, use
// `ctx_.primitiveTypeRef(sema::PrimitiveType::Char)` as `elemTypeRef` instead
// of the generic-collection derivation — the same `char` TypeRef
// `lowerToLazyEnumerator`'s own string arm already builds for the identical
// reason (`LlvmBackend_Calls3.cpp`, fix #1220). With that fix, the empty-
// string rows below correctly print `True`.
//
// PRE-EXISTING, SEPARATE BUG FOUND WHILE WRITING THIS FIX (NOT fixed here,
// filed as a new issue): `s.FirstOrNull()`/`LastOrNull()`/`SingleOrNull()` —
// ALREADY-admitted string-receiver operators, untouched by this fix — hit
// the SAME `elemTypeRef == nullptr` defect at their own, separate call
// sites (`LlvmBackend_Calls.cpp` ~4131/4358/4437), and go further: force-
// unwrapping the result with `!` SEGFAULTS THE COMPILER (not the compiled
// program), because sema types the local as `char?` (the tagged-struct
// LLVM type) while codegen produced a bare i32 for it. Measured: `var s :=
// "banana"; var f := s.FirstOrNull(); var g := f!;` crashes `bezelc` itself
// (SIGSEGV, confirmed on this tree with NONE of this lane's edits present —
// this file's own MinOrNull/MaxOrNull arm fix does not touch that code
// path). This is a materially different bug from this file's scope (element
// nullable-encoding vs. value-type detection have been split across two
// unrelated code paths since fix #1220 first admitted string, and only one
// of the two — this lane's — was corrected). Out of scope for #3786 (this
// lane owns `BuiltinRegistry.cpp` and the MinOrNull/MaxOrNull arm only).
//
// `ElementAtOrNull`/`OfType` (the OTHER two names #3444 flagged) are NOT
// added here — `ElementAtOrNull` has no lowering on any receiver (a
// separate, larger gap per #3444/#3764) and `OfType` was removed from the
// stdlib entirely at v0.284 (issue #3559) — both stay gated and are pinned
// still-rejected below.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone.
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

std::string prog(const std::string& body) {
    return
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// ===========================================================================
// The issue's OWN repro (GH #3786), strengthened per the #3764 lesson: the
// null-check text alone does not discriminate a "reject vs silent wrong
// value" bug — but here the pre-fix behavior is a REJECT (the table simply
// doesn't admit the name), so a successful compile producing the right
// stdout already discriminates. The third line (the unwrapped code point)
// additionally guards against a future partial-revert that keeps the two
// names in the table but drops the `elemTypeRef` codegen fix, which would
// compile successfully but print the wrong `null`-check result.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_string_receiver_3786, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M;\n"
            "import System;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var s := \"banana\";\n"
            "    var mn := s.MinOrNull();\n"
            "    Console.WriteLine(mn = null);\n"
            "    Console.WriteLine(int(mn!));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "False\n97");
}

// ===========================================================================
// §6b enumeration: non-empty, empty, single char, non-ASCII (code points,
// not UTF-8 bytes — §4.5: char is a 32-bit Unicode scalar value; string
// iterates by code point per §9.6/fix #1220 — matching what `First()`/
// `Min()` already do on a string, unaffected by this fix), a chain, and
// the other-operators/still-gated control rows.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_string_receiver_3786, non_empty_min_max) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    var mn := s.MinOrNull();\n"
            "    var mx := s.MaxOrNull();\n"
            "    Console.WriteLine(int(mn!));\n"
            "    Console.WriteLine(int(mx!));")),
        "97\n110");
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786, empty_returns_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"\";\n"
            "    var mn := s.MinOrNull();\n"
            "    var mx := s.MaxOrNull();\n"
            "    Console.WriteLine(mn = null);\n"
            "    Console.WriteLine(mx = null);")),
        "True\nTrue");
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786, single_char) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"z\";\n"
            "    var mn := s.MinOrNull();\n"
            "    var mx := s.MaxOrNull();\n"
            "    Console.WriteLine(int(mn!));\n"
            "    Console.WriteLine(int(mx!));")),
        "122\n122");
}

// Non-ASCII: 'a' (U+0061=97), 'e' with acute (U+00E9=233), an emoji
// (U+1F600=128512), 'b' (U+0062=98). Min is 'a' (the smallest code point),
// Max is the emoji (the largest) — proving the comparison runs over CODE
// POINTS (matching First()/Min()'s established §9.6 behavior), not UTF-8
// bytes (which would order differently: a multi-byte UTF-8 sequence's
// continuation bytes are all >= 0x80, but its LEAD byte for U+00E9 is 0xC3 =
// 195, still less than the emoji's 4-byte lead 0xF0=240, so a naive
// byte-max would coincidentally agree here — the code-point value is the
// real oracle, printed directly via `int(char)` so there is no
// byte-vs-code-point ambiguity in the assertion itself, per lane rules).
BEZEL_TEST(minornull_maxornull_string_receiver_3786, non_ascii_codepoints) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"a\\u00E9\\U0001F600b\";\n"
            "    var mn := s.MinOrNull();\n"
            "    var mx := s.MaxOrNull();\n"
            "    Console.WriteLine(int(mn!));\n"
            "    Console.WriteLine(int(mx!));")),
        "97\n128512");
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786, chain_where_then_min) {
    // "banana" chars > 'a' (97): b(98) n(110) n(110) n(110) -> min 98.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    var mn := s.Where(lambda(c: char) => c > 'a').MinOrNull();\n"
            "    Console.WriteLine(int(mn!));")),
        "98");
}

// Control: the other 37 already-admitted string-receiver operators are
// unaffected (byte-identical dispatch — this fix adds two names to the
// table and specializes `elemTypeRef` ONLY inside the MinOrNull/MaxOrNull
// arm). `FirstOrNull`'s bare `!`-unwrap is deliberately NOT exercised here
// (see the file header — that is the separate, pre-existing crash bug this
// lane found and filed, not this fix's regression).
BEZEL_TEST(minornull_maxornull_string_receiver_3786,
           other_string_operators_still_route_the_same) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    Console.WriteLine(s.Sum(lambda(c: char) => int(c)));\n"
            "    Console.WriteLine(int(s.First()));\n"
            "    Console.WriteLine(int(s.Min()));\n"
            "    Console.WriteLine(int(s.Max()));\n"
            "    var f := s.FirstOrNull();\n"
            "    Console.WriteLine(f = null);")),
        "609\n98\n97\n110\nFalse");
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786,
           still_gated_element_at_or_null) {
    // stdlib §5, fix #1220: ElementAtOrNull has no lowering on ANY receiver
    // (a separate, larger gap per issue #3444/#3764) — must stay rejected.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        prog("    var s := \"abc\";\n"
             "    var e := s.ElementAtOrNull(0);"),
        "not yet supported in codegen"));
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786, still_gated_of_type) {
    // `OfType` was REMOVED from the stdlib at v0.284 (issue #3559) — must
    // stay rejected regardless of this fix.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        prog("    var s := \"abc\";\n"
             "    var arr := s.OfType<char>().ToArray();"),
        "not yet supported in codegen"));
}

// ===========================================================================
// Leak-amplified (lane rules §R12-G / §3): the materialized char-array
// temporary (`bz_str_to_char_array` + `bz_lazy_to_array`) is freed exactly
// once per terminal call (`freeLazyTemp()`), before the call returns to the
// loop body — a leaked or double-freed snapshot shows up under ASan/LSan
// over 200 iterations. `runAotChecked` is ASan/LSan-instrumented
// independently of the plain-oracle gtest run.
// ===========================================================================

BEZEL_TEST(minornull_maxornull_string_receiver_3786,
           leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var s := \"banana\";\n"
        "      var mn := s.MinOrNull();\n"
        "      var mx := s.MaxOrNull();\n"
        "      total := total + int(mn!) + int(mx!);\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    // (97 + 110) * 200 = 41400.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=41400\n"));
}

BEZEL_TEST(minornull_maxornull_string_receiver_3786,
           empty_leak_free_200x) {
    auto r = runAotChecked(prog(
        "    var nullCount := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var s := \"\";\n"
        "      var mn := s.MinOrNull();\n"
        "      var mx := s.MaxOrNull();\n"
        "      if mn = null and mx = null then\n"
        "        nullCount := nullCount + 1;\n"
        "      end;\n"
        "    end;\n"
        "    Console.WriteLine($\"nullCount={nullCount}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nullCount=200\n"));
}

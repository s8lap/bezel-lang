// Spec-derived regression test for issue #2758 — INDEX ARGUMENTS ARE
// POSITIONAL-ONLY (language spec §23.5 "Restrictions", v0.241; owner ruling
// direction (1), PR #3043; Appendix A.4 `Argument` / `IndexerDecl` comments).
//
// §23.5, verbatim (v0.241):
//
//   "Index arguments are **positional-only**. The named-argument form
//    (`Identifier :=`, §22.5) is not admitted at an index position — a compile
//    error naming this rule. Indexer parameters may not declare default values,
//    so §22.5's optional-parameter and named-binding machinery is never reached
//    from an index position; an indexer's parameter names are
//    declaration-internal, not caller-bindable API surface (consistent with
//    §23.4's overloading by parameter *type* list)."
//
// WHY THE FORM PARSES AT ALL. A.7's `PrimarySuffix ::= "[" ArgList "]"` shares
// the CALL argument grammar, and `Parser::parseArgList` is literally the same
// function for `f(...)`, `new T(...)` and `x[...]` (Parser_Exprs.cpp). A.4's
// `Argument` comment records the resulting over-admission: "Named arguments are
// additionally rejected at the index position (§23.5) — admitted by the shared
// grammar, rejected semantically."
//
// ── WHAT THIS PINS, AND WHY THE DIAGNOSTIC TEXT IS THE DISCRIMINATOR ────────
//
// `runAot`'s WIFSIGNALED branch synthesizes an Error diagnostic for a runtime
// SIGSEGV, and — more importantly here — a CODEGEN failure is also reported as
// an error. Pre-fix, `d[fallback := 1, key := "abcd"]` did not compile clean:
// the `int` bound POSITIONALLY to the `string` slot and `bezelc` died in the
// LLVM verifier. So `r.diagnostics.hasErrors()` alone is NOT the pin — it was
// already true for that one shape. Every REJECT case below therefore asserts
// the specific §23.5 diagnostic TEXT the fix emits.
//
// MEASURED ON 309356e (the parent), with `build/bezelc -c`:
//
//   d[key := "abcd", fallback := 1]   compiled CLEAN, no diagnostic; the names
//                                     were dropped and the arguments bound by
//                                     position (they happened to agree)
//   d["abcd", fallback := 1]          same
//   d[fallback := 1, key := "abcd"]   "GEP base pointer is not a vector or a
//                                     vector of pointers" / "Load operand must
//                                     be a pointer" / "Call parameter type does
//                                     not match function signature" →
//                                     "bezelc: codegen error: IR verification
//                                     failed" — an internal compiler error
//                                     where §23.5 asks for a diagnostic
//   o[i := 3] / o[i := 3] := 9        compiled clean
//   a[index := 0], m[key := "k"],
//   s[i := 0], ia[i := 1]             compiled clean (built-in receivers)
//   this[i := 4], base[i := 4]        compiled clean
//   property [j: int := 0]: int       compiled clean (declaration side)
//
// ── ANTI-OVER-REJECT ────────────────────────────────────────────────────────
//
// #2758 warns in its own text that "this is a rejection-or-resolution change,
// so the false-reject risk is the hazard: #2709, #2713 and #2722 each took
// multiple rounds on exactly that this run." The ACCEPT half below therefore
// asserts VALUES, not exit-0: a positional index that silently degraded to a
// zero-initialized value would still "compile and run", and only a value
// oracle distinguishes that from working. Each accept case was measured on the
// FIXED compiler at the value asserted.
//
// The clean-compile accept surface is broader still and lives in
// tests/lit/index_positional_accept_2758.bzl; the per-shape diagnostic text is
// pinned in tests/lit/index_named_argument_reject_2758.bzl.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The two indexer shapes every case below draws on: a two-parameter indexer
// (where a REORDER is expressible) and a one-parameter one with both accessors
// (where it is not, and the form is still rejected).
const char* kDecls =
    "module M; "
    "class Dict "
    "  public property [key: string, fallback: int]: int "
    "    get => key.Length + fallback; "
    "  end; "
    "end Dict; "
    "class One "
    "  public property [i: int]: int "
    "    get => i * 2; "
    "    set => Console.WriteLine(value); "
    "  end; "
    "end One; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "class C public static method Main(): int " + body + " end Main; end C;";
}

// The discriminator: the §23.5 rejection's own text. NOT `hasErrors()` — see
// the header for why that is already true pre-fix for the reordered shape.
bool rejectsAsNamedIndex(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "not admitted at an index position") != std::string::npos &&
            d.message.find("\xC2\xA7""23.5") != std::string::npos)
            return true;
    }
    return false;
}

// The same, for the declaration-side half of the §23.5 bullet.
bool rejectsIndexerParamDefault(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("may not declare a default value") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""23.5") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── REJECT: the named form at an index position ─────────────────────────────

BEZEL_TEST(index_named_arg_positional_only_2758,
           all_named_on_a_two_parameter_indexer_is_rejected_spec_23_5) {
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(prog(
        "var d := new Dict(); return d[key := \"abcd\", fallback := 1];")));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           a_mixed_positional_and_named_index_is_rejected_spec_23_5) {
    // §22.5 admits "named arguments … in any order after positional arguments"
    // at a CALL. §23.5 admits none of it at an index.
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(prog(
        "var d := new Dict(); return d[\"abcd\", fallback := 1];")));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           the_reordered_named_index_no_longer_reaches_the_llvm_verifier_spec_23_5) {
    // THE shape #2758 is about: pre-fix this bound POSITIONALLY, put an `int`
    // in the `string` slot, and died in the LLVM verifier. Post-fix it is a
    // §23.5 diagnostic, and no binary is produced.
    const std::string p = prog(
        "var d := new Dict(); "
        "Console.WriteLine(\"ran\"); "
        "return d[fallback := 1, key := \"abcd\"];");
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(p));
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ran") == std::string::npos);
    // The malformed-IR failure is gone: sema stops the program before codegen.
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("IR verification failed") ==
                          std::string::npos);
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           a_single_parameter_indexer_is_rejected_too_spec_23_5) {
    // No reorder is expressible here — the rule is that the parameter names are
    // not caller-bindable API surface at all, not that a reorder is ambiguous.
    BEZEL_EXPECT_TRUE(
        rejectsAsNamedIndex(prog("var o := new One(); return o[i := 3];")));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           the_setter_side_is_rejected_spec_23_4_23_5) {
    // §23.4's indexer is ONE member with two accessors, and §23.5's rule
    // governs the index list — so it cannot depend on which accessor runs.
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        prog("var o := new One(); o[i := 3] := 9; return 0;")));
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        prog("var o := new One(); o[i := 4] += 1; return 0;")));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           builtin_receiver_families_are_rejected_spec_23_5) {
    // None of these declares a user indexer for a name to bind against; each
    // mapped `indices[0]` to its one index slot positionally and dropped the
    // name silently.
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(prog(
        "using var a := new Array<int>(); a.Add(7); return a[index := 0];")));
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(prog(
        "using var m := new Dictionary<string,int>(); m[\"k\"] := 5; "
        "return m[key := \"k\"];")));
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        prog("var s := \"abc\"; return int(s[i := 0]);")));
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        prog("var ia: [3]int := [1,2,3]; return ia[i := 1];")));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           this_and_base_indexer_access_are_rejected_spec_23_4_12_3) {
    // `this[i]` is the spelling §23.4 REQUIRES inside a member of a class with
    // its own indexer; `base[i]` is §12.3's base-qualified access. Both are
    // IndexExpr nodes whose target carries no ordinary exprType, which is why
    // they are checked in the receiver-independent walk rather than beside the
    // receiver-typed #2619 check.
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        "module M; "
        "class S "
        "  public property [i: int]: int => i + 1; "
        "  public method Get(): int return this[i := 4]; end Get; "
        "end S; "
        "class C public static method Main(): int "
        "  var s := new S(); return s.Get(); end Main; end C;"));
    BEZEL_EXPECT_TRUE(rejectsAsNamedIndex(
        "module M; "
        "class B public virtual property [i: int]: int => i + 1; end B; "
        "class D: B "
        "  public override property [i: int]: int => i + 2; "
        "  public method Get(): int return base[i := 4]; end Get; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Get(); end Main; end C;"));
}

// ── REJECT: the declaration-side half of the same §23.5 bullet ──────────────

BEZEL_TEST(index_named_arg_positional_only_2758,
           an_indexer_parameter_may_not_declare_a_default_spec_23_5) {
    // "Indexer parameters may not declare default values, so §22.5's
    // optional-parameter and named-binding machinery is never reached from an
    // index position." A.4's IndexerDecl comment records the shared-ParamList
    // over-admission alongside the `static` one. Pre-fix this compiled clean
    // and the default was simply unreachable — there is no `[]` form.
    BEZEL_EXPECT_TRUE(rejectsIndexerParamDefault(
        "module M; "
        "class Q public property [i: int := 0]: int => i + 10; end Q; "
        "class C public static method Main(): int "
        "  var q := new Q(); return q[1]; end Main; end C;"));
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           a_method_parameter_default_is_untouched_spec_22_5) {
    // The ban is on INDEXER parameters only; §22.5's optional parameters on
    // ordinary methods keep working, value and all.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Bump(n: int, by: int := 5): int return n + by; "
        "  end Bump; "
        "  public static method Main(): int return Bump(37); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── ACCEPT: every positional spelling still produces the RIGHT VALUE ────────

BEZEL_TEST(index_named_arg_positional_only_2758,
           positional_indexing_of_a_user_indexer_still_works_spec_23_4) {
    // Two-parameter get, one-parameter get, and the setter (which prints).
    auto r = runAot(prog(
        "var d := new Dict(); "
        "var o := new One(); "
        "o[3] := 9; "
        "return d[\"abcd\", 1] + o[3];"));            // (4+1) + 6 == 11
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(11, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           an_index_value_that_is_a_local_named_key_is_not_the_named_form) {
    // The one shape that could be confused for `key := …` and is not: the
    // parser takes the named prefix ONLY when `:=` follows the identifier.
    auto r = runAot(prog(
        "var d := new Dict(); var key := \"abcd\"; return d[key, 1];"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           a_trailing_comma_in_the_index_list_still_works_spec_1_6) {
    // §1.6 / proposal 0059 (v0.214): the optional trailing `,` before `]`.
    auto r = runAot(prog("var d := new Dict(); return d[\"abcd\", 1,];"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           builtin_receiver_families_still_index_positionally) {
    auto r = runAot(prog(
        "using var a := new Array<int>(); a.Add(7); "
        "var sp := a.AsSpan(); "
        "using var m := new Dictionary<string,int>(); m[\"k\"] := 5; "
        "var s := \"abc\"; "
        "var ia: [3]int := [1,2,3]; "
        "return a[0] + sp[0] + m[\"k\"] + int(s[0]) + ia[1];"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7 + 7 + 5 + 97 + 2, r.exitCode);   // 118
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           this_and_base_positional_indexing_still_work_spec_23_4_12_3) {
    auto r = runAot(
        "module M; "
        "class B public virtual property [i: int]: int => i + 1; end B; "
        "class D: B "
        "  public override property [i: int]: int => i + 2; "
        "  public method Both(): int return this[4] + base[4]; end Both; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Both(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(11, r.exitCode);                   // this[4]==6, base[4]==5
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           overloaded_generic_and_record_indexers_still_work_spec_23_4) {
    // §23.4: "Indexers may be overloaded by parameter type list" and "Records
    // may declare read-only indexers".
    auto r = runAot(
        "module M; "
        "class Ov "
        "  public property [i: int]: int => i; "
        "  public property [s: string]: int => s.Length; "
        "end Ov; "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end; "
        "  public property [i: int]: T => _v; "
        "end Box; "
        "record Rec(BaseValue: int) "
        "  public property [i: int]: int => BaseValue + i; "
        "end Rec; "
        "class C public static method Main(): int "
        "  using var v := new Ov(); "
        "  using var b := new Box<int>(5); "
        "  var r := new Rec(3); "
        "  return v[1] + v[\"ab\"] + b[0] + r[2]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1 + 2 + 5 + 5, r.exitCode);        // 13
}

BEZEL_TEST(index_named_arg_positional_only_2758,
           the_named_form_at_a_call_and_a_new_is_untouched_spec_22_5) {
    // The other direction: §22.5's named arguments are a CallExpr / NewExpr
    // property and must keep working. A rejection keyed on the node kind
    // rather than on `CallArgument::name` would have broken these.
    auto r = runAot(
        "module M; "
        "class Cfg "
        "  public readonly property N: int; "
        "  public constructor(verbose: bool, n: int) N := n; end; "
        "end Cfg; "
        "class C "
        "  public static method Add(a: int, b: int): int return a + b; end Add; "
        "  public static method Main(): int "
        "    using var cfg := new Cfg(verbose := true, n := 3); "
        "    return Add(b := 2, a := 1) + cfg.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(6, r.exitCode);
}

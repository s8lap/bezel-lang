// Spec-derived regression test for issue #3097 — INDEX ARGUMENTS ARE NEVER
// MODIFIERED (language spec §23.5 "Restrictions", v0.249; standing text from
// #2758's ruling; Appendix A.4 `Argument` comment; Node.hpp
// `CallArgument::modifier`).
//
// §23.5, verbatim (v0.249):
//
//   "`ref` and `out` parameters are not allowed in indexer parameter lists."
//
// Per #2758's ruling, "An index argument (the operand of `[...]`) shall not
// carry a ref/out/owned/scoped modifier; ownership of an index argument is
// determined by the indexer's declared parameter ownership." The standing
// text settles the spec; this test pins the compiler's enforcement of it.
//
// The four SPEC-LISTED modifier spellings (`ref`, `out`, `owned`, `scoped`)
// each parse into a `CallArgument::modifier` (#1439: §22.3 / §28.14) and were
// silently accepted on c6a5355 — measured by the issue:
//
//   d[ref i]      compiles clean (the named-argument fix does not touch
//   d[out i]      modifiers; `a.modifier.empty()` was the guard #2613's
//   d[owned i]    nullability check carried too, so the nullability fix never
//   d[scoped i]   fired for any of these spellings)
//   d[out _]      parses; codegen degrades to a zero/null return value with
//                 the `internal: … (degraded to a zero/null return value) —
//                 #2491` diagnostic — the wrong reason for a spec rule
//   d[out var i]  rejected with `unresolved identifier i` — the wrong reason
//                 for a spec rule (the same call-site shape that #2657 /
//                 #2668 / #3186 / #3188 made legal at method and constructor
//                 calls never reached sema at an index position)
//   d[name := x]  rejected with §23.5 (the #2758 arm of the rule, which this
//                 test does NOT exercise — it lives in
//                 tests/sema/index_named_arg_positional_only_2758_test.cpp)
//
// ── WHAT THIS PINS ──────────────────────────────────────────────────────────
//
// REJECTION: every modifier spelling — `ref`, `out`, `out var`, `out using
// var`, `out owned`, `out owned var`, `owned`, `scoped` — produces a §23.5
// diagnostic. The discriminator is the diagnostic TEXT, not exit-1 — a
// widened predicate that fires on the wrong rule would also exit non-zero,
// so the §23.5 cite and the modifier-name mention are what pin the fix.
//
// ACCEPT: the UNMODIFIED forms — a plain local `d[k]`, a literal `d["x"]`,
// the setter side `o[i] := v` — continue to compile CLEAN, with the right
// VALUES on the read. The fix must NOT widen to the named-argument arm (the
// #2758 fix already enforces that separately, and a doubled diagnostic for a
// `d[k := "x"]` shape is what #2758's ACCEPT controls guard against — this
// fix does not touch that arm).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A one-parameter indexer, like #3097's repro. `get` returns the key's length
// (a deterministic value); `set` ignores the value.
const char* kDecls =
    "module M; "
    "class Dict "
    "  public property [k: string]: int "
    "    get => k.Length; "
    "    set => Console.WriteLine(value); "
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
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// The discriminator: the §23.5 rejection's own text, naming the modifier it
// was attached to. NOT `hasErrors()` — see the file header for why that is
// already true pre-fix for the `out _` shape (which degraded at codegen
// behind an `internal: …` message) and pre-fix could be silenced on the
// `out var i` shape by the `unresolved identifier i` path. Both also
// `hasErrors()`; neither is the §23.5 diagnostic. The check below matches
// the modifier NAME the issue lists AND the §23.5 cite the fix emits.
bool rejectsModifier(const std::string& program, const char* modifier) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("index argument may not carry a `") ==
            std::string::npos) continue;
        if (d.message.find(modifier) == std::string::npos) continue;
        if (d.message.find("\xC2\xA7""23.5") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ── REJECT: the four SPEC-LISTED modifier spellings (§23.5, v0.249) ────────

BEZEL_TEST(index_arg_ownership_modifier_3097,
           owned_index_argument_is_rejected_spec_23_5) {
    // The §28.1 / §28.14 closed-positions list forbids `owned` here: the
    // indexer's parameter side is closed by §23.5, so the argument-side
    // ownership is determined by the declared parameter's modifier and an
    // `owned` modifier names nothing on either side.
    const std::string p = prog(
        "var d := new Dict(); var k := \"abcd\"; return d[owned k];");
    BEZEL_EXPECT_TRUE(rejectsModifier(p, "owned"));
    // Pre-fix the program silently accepted; the fix also narrows the
    // ownership pass (`coll[owned r]` was classified as a live consume form
    // at #2758-era ownership code, not as an error) — the value-oracle
    // therefore asserts no value is produced.
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           ref_index_argument_is_rejected_spec_23_5) {
    // §23.5: "`ref` and `out` parameters are not allowed in indexer parameter
    // lists." An indexer has no `ref` parameter for a `ref` argument to match.
    const std::string p = prog(
        "var d := new Dict(); var k := \"abcd\"; return d[ref k];");
    BEZEL_EXPECT_TRUE(rejectsModifier(p, "ref"));
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           out_index_argument_is_rejected_spec_23_5) {
    // Same §23.5 clause, `out` arm. The wrong-reason surface — `out _`
    // previously degraded at codegen — is closed by the same predicate.
    const std::string p = prog(
        "var d := new Dict(); var k := \"abcd\"; return d[out k];");
    BEZEL_EXPECT_TRUE(rejectsModifier(p, "out"));
    // The discard spelling `d[out _]` falls under the same rule; pre-fix it
    // was accepted at sema and degraded at codegen.
    BEZEL_EXPECT_TRUE(rejectsModifier(prog(
        "var d := new Dict(); return d[out _];"), "out"));
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           scoped_index_argument_is_rejected_spec_23_5) {
    // §28.14 closes `scoped` at the same set of positions §28.1 closes
    // `owned`, and §23.5's parameter-side ban is what makes the argument
    // side meaningful at all.
    const std::string p = prog(
        "var d := new Dict(); var k := \"abcd\"; return d[scoped k];");
    BEZEL_EXPECT_TRUE(rejectsModifier(p, "scoped"));
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           out_var_index_argument_is_rejected_with_section_23_5) {
    // Pre-fix this rejected, but for `unresolved identifier i`. The fix
    // re-points the rejection to §23.5 — the actual rule the spec lists —
    // so the §23.5 cite MUST be present even though `unresolved identifier`
    // may also fire (the `var` declares a local the binding never reaches).
    auto r = runAot(prog(
        "var d := new Dict(); return d[out var k];"));
    bool saw3097 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`out var`") != std::string::npos &&
            d.message.find("\xC2\xA7""23.5") != std::string::npos) {
            saw3097 = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(saw3097);
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           builtin_receiver_families_are_rejected_too_spec_23_5) {
    // The rule is receiver-independent (checkIndexArgsPositional runs once
    // per IndexExpr from visitChildren's IndexExpr arm, before the receiver
    // family is resolved), so the same diagnostic must fire on `Array<T>`,
    // `Dictionary<K,V>`, `string`, and an inline array — each of which
    // declares a non-user indexer and was therefore a clean accept surface
    // for every modifier spelling pre-fix.
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); var i := 0; "
        "  return a[owned i]; "
        "end Main; end C;", "owned"));
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class C public static method Main(): int "
        "  using var m := new Dictionary<string,int>(); var k := \"k\"; "
        "  return m[owned k]; "
        "end Main; end C;", "owned"));
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"abc\"; var i := 0; "
        "  return int(s[ref i]); "
        "end Main; end C;", "ref"));
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class C public static method Main(): int "
        "  var ia: [3]int := [1,2,3]; var i := 1; "
        "  return ia[scoped i]; "
        "end Main; end C;", "scoped"));
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           this_and_base_indexer_access_are_rejected_too_spec_23_4_12_3) {
    // Same IndexExpr node, same per-argument walk — `this[i]` (§23.4
    // requires it inside a class with its own indexer) and `base[i]` (§12.3)
    // reach the check the same way. The receiver-typed path does not.
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class S "
        "  public property [i: int]: int => i + 1; "
        "  public method Get(): int "
        "    var i := 4; return this[owned i]; "
        "  end Get; "
        "end S; "
        "class C public static method Main(): int "
        "  var s := new S(); return s.Get(); end Main; end C;", "owned"));
    BEZEL_EXPECT_TRUE(rejectsModifier(
        "module M; "
        "class B public virtual property [i: int]: int => i + 1; end B; "
        "class D: B "
        "  public override property [i: int]: int => i + 2; "
        "  public method Get(): int "
        "    var i := 4; return base[out i]; "
        "  end Get; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Get(); end Main; end C;", "out"));
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           the_setter_side_is_rejected_too_spec_23_4_23_5) {
    // §23.4's indexer is ONE member with two accessors; §23.5's rule
    // governs the index list and cannot depend on which accessor runs.
    BEZEL_EXPECT_TRUE(rejectsModifier(
        prog("var o := new One(); var i := 3; o[owned i] := 9; return 0;"),
        "owned"));
    BEZEL_EXPECT_TRUE(rejectsModifier(
        prog("var o := new One(); var i := 3; o[ref i] += 1; return 0;"),
        "ref"));
}

// ── ACCEPT: every POSITIONAL spelling still produces the RIGHT VALUE ───────

BEZEL_TEST(index_arg_ownership_modifier_3097,
           a_plain_local_index_argument_still_works_spec_23_4) {
    // The plain value-bearing spelling — exactly what the modifier check
    // guards. Must compile clean and produce the right value.
    auto r = runAot(prog(
        "var d := new Dict(); var k := \"abcd\"; return d[k];"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(4, r.exitCode);                  // k.Length == 4
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           a_literal_index_argument_still_works_spec_23_4) {
    // The one shape that could be confused for `key := …` (the #2758 arm)
    // and is NOT — the parser takes the named prefix ONLY when `:=` follows
    // the identifier. A literal `"abcd"` is just an expression.
    auto r = runAot(prog(
        "var d := new Dict(); return d[\"abcd\"];"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(4, r.exitCode);
}

BEZEL_TEST(index_arg_ownership_modifier_3097,
           the_named_argument_form_at_a_call_and_a_new_is_untouched_spec_22_5) {
    // The OTHER direction: the named form is a §22.5 CALL/NEW property and
    // must keep working. The fix is keyed on the IndexExpr node kind (the
    // walk in visitChildren's IndexExpr arm), not on `CallArgument::name` —
    // so a doubled diagnostic here would have been a regression.
    auto r = runAot(
        "module M; "
        "class Cfg "
        "  public readonly property N: int; "
        "  public constructor(verbose: bool, n: int) N := n; end; "
        "end Cfg; "
        "class C "
        "  public static method Add(a: int, b: int): int return a + b; "
        "  end Add; "
        "  public static method Main(): int "
        "    using var cfg := new Cfg(verbose := true, n := 3); "
        "    return Add(b := 2, a := 1) + cfg.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(6, r.exitCode);
}
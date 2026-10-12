// fix #3842 — the `out owned var` CALL-SITE spelling is retired (spec §A.4 /
// §22.4, v0.289 L2; proposal 0090, v0.246 for the bare `owned` deletion).
//
// Appendix A.4 reads, in full:
//
//   ArgModifiers ::= "ref"
//                  | "out" "var"?
//                  | "out" "using" "var"
//                  | "scoped"
//
// There is NO `owned` alternative. Proposal 0090 (v0.246) deleted the bare
// `owned` alternative and moved the call-site transfer marker into the
// argument EXPRESSION (`M(move x)`, §22.3 / §28.8.1). v0.289 L2 deleted the
// LAST use-site `owned` — `out owned var x` — because `out var x` already
// declares a fresh local that ADOPTS the classification the resolved parameter
// announces (§22.4's adoption clause; §28.3), so the marker was redundant
// rather than informative.
//
// LAYER. The rejection belongs to the PARSER, not to a later pass: `out owned
// var` is not derivable from the A.4 `ArgModifiers` production, so it is a
// SYNTAX error by the same token that rejects any other non-derivable spelling.
// `Parser::parseArgList`'s `out` branch was the sole producer of the
// `CallArgument::modifier` string "out owned var" (MEASURED: `git grep
// 'a.modifier = '` — every other writer targets a declaration-side Param or a
// FuncParamMods slot). These rows therefore pin BOTH halves: the retired
// spelling is rejected from the PARSER LAYER (an explicit `parseAll` row, which
// fails if a later pass were the one rejecting, or if the parser regressed to
// admitting it), and the two sanctioned successors are accepted.
//
// The three syntactic positions are separate rows because A.4's `Argument`
// production is reached from three call sites of `parseArgList`, and a
// retirement that covers only two of them is a HALF-landed retirement — the
// exact failure mode #3842 recorded when the OWNERSHIP pass retired the
// spelling while the parser kept admitting it and the tree stayed green:
//   - `Parser_Exprs.cpp:708`     `new T(...)`       (constructor call)
//   - `Parser_Exprs.cpp:2191`    call postfix `(...)` (method call)
//   - `Parser_Decls.cpp:331`     `this(...)` / `base(...)` (ctor chain)
//
// The FAIL-WITHOUT is mechanical: at BASE (b544e6d4, before this fix) all three
// positions COMPILE CLEANLY — `bezelc prog.bzl -o <real path>` returns rc=0 —
// and the `out var` / `out using var` successors compile at BASE too, so the
// accept rows are not vacuous and the reject rows are not pre-empted by an
// unrelated earlier rule.
//
// per §A.4, §22.3, §22.4, §28.3, §28.8.1; #3842, #3056, #1305

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::ParseResult;
using bezel::test::parseAll;

namespace {

// A minimal module whose method body is the call under test. `body` is spliced
// verbatim, so each row below reads as the Bezel a user would have written.
std::string programWith(const std::string& body) {
    return "module M; class Res public constructor() end constructor; "
           "destructor end destructor; end Res; "
           "class Sink public constructor(out owned n: Res) n := new Res(); "
           "end constructor; public static method TryMake(out owned n: Res): "
           "bool n := new Res(); return true; end TryMake; end Sink; "
           "class C public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

// The diagnostic text the retirement reports. Pinned in full because the
// message is the user's only route to the sanctioned replacement — the point
// of retiring a spelling is that the error names what to write instead.
const char* kRetiredNeedle = "is not a call-site spelling";

bool anyDiagnosticMentions(const ParseResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Does any reported diagnostic carry this needle? Used to prove the rejection
// is THIS rule firing, not an unrelated earlier one (the pre-emption trap).
bool rejectsWith(const ParseResult& r, const std::string& needle) {
    if (!r.diagnostics.hasErrors()) return false;
    return anyDiagnosticMentions(r, needle);
}

}  // namespace

// ===========================================================================
// THE RETIREMENT — constructor call position (`new T(...)`,
// `Parser_Exprs.cpp:708`). The issue's own repro shape.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842,
           new_expr_rejects_retired_spelling_from_parser) {
    ParseResult r = parseAll(programWith(
        "var w := new Sink(out owned var n); n.Dispose(); w.Dispose(); "));
    // The spelling is not derivable from A.4 — a parse-layer error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // ... and it is THIS rule, not an unrelated earlier one.
    BEZEL_EXPECT_TRUE(rejectsWith(r, kRetiredNeedle));
    // The remedy is named, so the diagnostic is actionable.
    BEZEL_EXPECT_TRUE(anyDiagnosticMentions(r, "out var"));
}

// ===========================================================================
// THE RETIREMENT — method-call position (`f(...)` postfix,
// `Parser_Exprs.cpp:2191`). Same spelling, third of three call sites of
// `parseArgList`; a retirement covering only `new` and this one would leave
// the chain-call arm live.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842,
           method_call_rejects_retired_spelling_from_parser) {
    ParseResult r = parseAll(programWith(
        "var ok := Sink.TryMake(out owned var n); n.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsWith(r, kRetiredNeedle));
}

// ===========================================================================
// THE RETIREMENT — ctor-chain position (`this(...)`, `Parser_Decls.cpp:331`).
// The fourth syntactic shape; `parseArgList` is shared, but the chain-call arm
// is the one #4975 gave its own codegen registration, so it is pinned
// separately rather than assumed covered.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842,
           this_chain_rejects_retired_spelling_from_parser) {
    ParseResult r = parseAll(
        "module M; class Res public constructor() end constructor; destructor "
        "end destructor; end Res; "
        "class WithSibling "
        "  public constructor(out owned r: Res) r := new Res(); "
        "  end constructor; "
        "  public constructor(unused: int) "
        "    this(out owned var recipient); "
        "    recipient.Dispose(); "
        "  end constructor; "
        "end WithSibling; "
        "class C public static method Main(): int "
        "  var d := new WithSibling(1); d.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsWith(r, kRetiredNeedle));
}

// ===========================================================================
// THE SUCCESSORS — `out var x` is admitted at the same `out owned` parameter
// (§22.4, v0.289 L2: the inline recipient ADOPTS the callee's owned
// classification). Without this row the reject rows above would be satisfied by
// a compiler that rejected `out var` too.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842, out_var_is_admitted_at_out_owned_parameter) {
    ParseResult r = parseAll(programWith(
        "var w := new Sink(out var n); n.Dispose(); w.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// THE SUCCESSORS — `out using var x`, the scope-bound owned form, unchanged by
// v0.289 L2 (it is accepted ONLY against `out owned`).
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842, out_using_var_is_admitted_at_out_owned_parameter) {
    ParseResult r = parseAll(programWith(
        "var w := new Sink(out using var n); w.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// The plain `out` and `out var` spellings at a PLAIN `out` parameter are
// untouched by the retirement — a narrow fix that only fired against `out
// owned` formals would leave this row broken.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842, plain_out_modifiers_are_untouched) {
    ParseResult r = parseAll(
        "module M; class Res public constructor() end constructor; destructor "
        "end destructor; end Res; "
        "class Plain public static method Get(out r: Res): bool "
        "  r := new Res(); return true; end Get; end Plain; "
        "class C public static method Main(): int "
        "  var ok := Plain.Get(out var r); r.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// BARE `owned` at an argument position was retired EARLIER (proposal 0090,
// v0.246) and must still be rejected — the retirement is layered, and this
// row keeps #3842's work from reading as the whole A.4 story. Its diagnostic
// is a different one (the `owned`-at-argument-position report), so this row
// pins the rejection without over-pinning the text.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842, bare_owned_at_argument_position_still_rejected) {
    ParseResult r = parseAll(
        "module M; class Res public constructor() end constructor; destructor "
        "end destructor; end Res; "
        "class Sink public static method Take(owned r: Res): bool "
        "  r := new Res(); return true; end Take; end Sink; "
        "class C public static method Main(): int "
        "  var g := new Res(); var ok := Sink.Take(owned g); g.Dispose(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// THE MIGRATION TARGET'S OWN HOLE — §28.1 erasure at a VALUE substitution.
//
// v0.289 L2 makes `out var x` the successor for every `out owned var x` site, so
// the retirement's migration lands users on it. At a bare-type-parameter
// substitution (§28.1 clause 3) the marker is ERASED, and §28.1's erasure is
// read off the recipient's SUBSTITUTED type kind — `isValueSubstitutionErased`
// deliberately does NOT erase a null-declaration Named, being the leak-safe
// direction.
//
// MEASURED ROOT CAUSE, pre-existing at BASE and independent of the retirement:
// the `out var` inline recipient took the BARE `T` as its `declType`, because
// ExprTypecheck_Exprs' CallExpr handler substituted the formal only for an
// INTERFACE parent (§11.8 / #807) — while `out using var`'s handler
// substitutes for ANY parent carrying generic params. The two spellings of the
// SAME §22.4 inline declaration therefore disagreed on the recipient's type,
// and the ownership pass kept an Owned classification for a value that carries
// no obligation: a spurious §28.3 must-use warning at `Factory<int>`, which
// `out owned var` (and `out using var`) did not produce.
//
// The companion rows that pin the RUNTIME halves of the same erasure gate are
// tests/ownership/out_owned_value_erasure_1860_test.cpp
// (`value_out_var_no_auto_drop_spec_28_1`, `value_out_var_runs_clean_spec_28_10`).
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842,
           out_var_recipient_types_at_generic_class_substitution) {
    // `n` is declared `out var` against `out owned result: T` on
    // `Factory<T>`; assigning it to an `int` local can only type-check if the
    // recipient carries the SUBSTITUTED type (`T` -> `int`), not a bare `T`.
    ParseResult r = parseAll(
        "module M; "
        "class Factory<T> "
        "  public method TryMake(owned v: T, out owned result: T): bool "
        "    result := move v; return true; "
        "  end TryMake; "
        "end Factory; "
        "class C public static method Main(): int "
        "  using var f := new Factory<int>(); "
        "  if f.TryMake(5, out var n) then "
        "    var x: int := n; "
        "    _ := x; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The class substitution must keep its OWNED classification — the widening of
// the substitution condition must not erase a recipient that really does carry
// a disposal obligation. This is the NEG boundary for the row above.
// ===========================================================================
BEZEL_TEST(out_owned_var_retired_3842,
           out_var_recipient_keeps_owned_at_generic_class_substitution) {
    // Same generic factory, instantiated at a CLASS type: the recipient is an
    // owned local and reading `.Id` off it is the class-substitution path. If
    // the substitution widening over-erased, this row's member access would
    // lose its type and the compile would fail.
    ParseResult r = parseAll(
        "module M; "
        "class Item public Id: int; public constructor(id: int) "
        "Id := id; end constructor; end Item; "
        "class Factory<T> "
        "  public method TryMake(owned v: T, out owned result: T): bool "
        "    result := move v; return true; "
        "  end TryMake; "
        "end Factory; "
        "class C public static method Main(): int "
        "  using var f := new Factory<Item>(); "
        "  if f.TryMake(new Item(3), out var n) then "
        "    _ := n.Id; "
        "    n.Dispose(); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
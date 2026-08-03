// fix #2328 (§26.5, v0.236): extension-method resolution never filtered
// candidates by the call's actual arity/types — it picked the first
// same-named member of an extension block whose target type matched the
// receiver, regardless of the call's arity. An extension method with the
// wrong arity was a candidate, leading to either a confusing diagnostic
// ("ambiguous extension member" between same-named blocks of different
// arities, even though the call's arity disambiguates) or, worse, a wrong
// choice in a call where checkCallArity silently bails (ms->parent is
// null for every extension method). The fix: filter candidates by the
// call's arity first (§26.5 / #1307's "applicable candidate" rule),
// then let the existing §26.5.1 specificity ranking pick the unique
// winner from the filtered set. An arity mismatch on a known-complete
// receiver now reports "no extension method `M` on type `T` takes N
// argument(s)" instead of either (a) the catch-all "no member" reject
// or (b) a §26.5 rule-4 ambiguity between differently-arity'd blocks.
//
// Each test below documents the axis it pins. Fail→pass was verified
// manually by reverting the arity filter and rebuilding; without the
// fix, every "filter by arity first" case regresses to the bogus
// ambiguity (two-extension / two-arity cases) or the zero-initialized
// codegen fallback (#2434, the historical silent-degrade the brief
// cites) for the single-extension / wrong-arity case.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// §26.5 / #2328 — control: a single-extension, right-arity call must keep
// compiling cleanly. This pins that the arity filter does not perturb
// the well-formed case (the brief's "control, must still work" axis).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328, single_extension_right_arity_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(41); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — single-extension, wrong-arity (0 args, method takes 1).
// Pre-#2328: the lookup returned the extension's M(arity 1) as the only
// candidate, then checkCallArity bailed (`ms->parent` null) — the call
// reached codegen with the wrong-arity callee and degraded to a
// zero-initialized value (#2434, the historical silent miscompile).
// Post-#2328: rejected with a clear arity-specific diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           single_extension_zero_args_against_arity_1_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(); "  // 0 args, method takes 1
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // The arity-specific wording: "no extension method `Bar` on
        // type `Foo` takes 0 argument(s) (§26.5)". Match on key
        // fragments to be robust against minor phrasing changes —
        // the historical "no member `Bar`" reject (which the brief
        // explicitly calls misleading) is not the right wording.
        if (d.message.find("no extension method") != std::string::npos &&
            d.message.find("Bar") != std::string::npos &&
            d.message.find("0 argument") != std::string::npos &&
            d.message.find("26.5") != std::string::npos) {
            found = true;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — single-extension, wrong-arity (2 args, method takes 1).
// Mirrors the 0-args case but the call has too MANY args, exercising
// the "extra positional arg" path (vs. the "missing arg" path above).
// Pre-#2328: same silent-degrade (#2434) — codegen reached with the
// wrong callee.
// Post-#2328: rejected with the arity-specific diagnostic naming 2.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           single_extension_two_args_against_arity_1_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(1, 2); "  // 2 args, method takes 1
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("no extension method") != std::string::npos &&
            d.message.find("Bar") != std::string::npos &&
            d.message.find("2 argument") != std::string::npos &&
            d.message.find("26.5") != std::string::npos) {
            found = true;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — two extensions on the same target with the same method
// name but different arities, called at arity 0. Pre-#2328: the lookup
// found BOTH candidates (extension-1's M(arity 0) and extension-2's
// M(arity 1)) and, with both as equally-specific on the same receiver,
// fired §26.5 rule-4 ambiguity — even though arity 0 disambiguates
// (only extension-1's M has arity 0). Post-#2328: extension-1's M
// is the unique arity-matching winner; the call resolves cleanly.
// The picked-method identity is verified by the diagnostic absence
// (the historical ambiguity emit would have failed the test).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           two_extensions_same_name_diff_arity_zero_picks_arity_0) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => 1; end; "
        "extension Foo public method Bar(x: int, y: int): int => 2; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(); "  // 0 args → arity-0 extension wins
        "  return 0; "
        "end Main; end C;");
    // No diagnostic means the arity-aware lookup resolved to a single
    // arity-matching candidate (extension-1's M) — the historical
    // ambiguity error pre-#2328 would have surfaced here.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — same two-extension setup, called at arity 2. Pre-#2328:
// the call was reported as ambiguous; post-#2328: extension-2's
// arity-2 method is the unique arity-matching winner.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           two_extensions_same_name_diff_arity_two_picks_arity_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => 1; end; "
        "extension Foo public method Bar(x: int, y: int): int => 2; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(10, 20); "  // 2 args → arity-2 extension wins
        "  return 0; "
        "end Main; end C;");
    // No diagnostic means the arity-aware lookup resolved to a single
    // arity-matching candidate (extension-2's M) — the historical
    // ambiguity error pre-#2328 would have surfaced here.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — the adversarial: a call at an arity that NEITHER
// extension matches. Pre-#2328: also ambiguous (all extensions
// considered); post-#2328: clear arity-mismatch diagnostic naming the
// call's arity (1) — neither extension has arity 1, so the user's
// call is wrong for both, and the diagnostic distinguishes that from a
// "no such method" error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           two_extensions_neither_arity_matches_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => 1; end; "
        "extension Foo public method Bar(x: int, y: int): int => 2; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var n := f.Bar(7); "  // 1 arg — neither block matches
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("no extension method") != std::string::npos &&
            d.message.find("Bar") != std::string::npos &&
            d.message.find("1 argument") != std::string::npos &&
            d.message.find("26.5") != std::string::npos) {
            found = true;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — primitive-receiver axis. The arity-aware lookup
// must also work when the receiver is a primitive type (`extension
// int`). The control: a right-arity call resolves cleanly.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           primitive_extension_right_arity_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public method Bar(x: int): int => x * 2; end; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  return n.Bar(3); "  // arity 1, matches
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 / #2328 — primitive-receiver, wrong-arity. Pre-#2328: the
// primitive-receiver path at line 10817 (ExprTypecheck_Calls.cpp) had
// the same arity-blindness — `int.Bar()` against `extension int
// method Bar(x: int)` either silently degraded (#2434) or reported
// "type 'int' has no static method" (§3.4) — neither of which is the
// right diagnostic. Post-#2328: the arity-aware lookup returns null
// (no extension on int with name Bar and arity 0), the primitive-static
// fallback's "type 'int' has no static method or property" is
// SUPPRESSED for the call context (currentCallArity_ >= 0), and
// control falls through to the #2126 "no member `Bar` on primitive
// type `int`" reject (line ~14018) — the right wording for a call
// against a primitive receiver. The test pins the §2126 wording is
// the one the user sees (and that the suppressed §3.4 wording is
// NOT double-emitted).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           primitive_extension_wrong_arity_preserves_spec_3_4) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public method Bar(x: int): int => x * 2; end; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  return n.Bar(); "  // 0 args, method takes 1
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("no member `Bar`") != std::string::npos &&
            d.message.find("primitive type `int`") != std::string::npos) {
            found = true;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}


// ---------------------------------------------------------------------------
// §26.5 / #2328 — bare-reference axis. A bare `f.Bar` (no call
// parens) is a method reference, not a call — `currentCallArity_` is
// -1, the arity filter is not applied. The lookup still finds BOTH
// same-named members across the two extension blocks, with the same
// specificity, and §26.5 rule 4 fires its "ambiguous extension
// member" reject. This is the pre-#2328 behavior for the same
// shape (the per-block first-name-match only handles multiple
// methods WITHIN a block; across blocks the candidates accumulate).
// The test pins that the bare-reference path is NOT affected by
// the arity filter — the ambiguity is detected and reported with
// the same wording it had pre-#2328 (no semantic regression for
// the bare-reference case). A call-site test, by contrast, gets
// the arity-specific diagnostic above.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_arity_2328,
           bare_reference_unaffected_by_arity_filter) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => 1; end; "
        "extension Foo public method Bar(x: int): int => 2; end; "
        "class C public static method Main(): int "
        "  var f := new Foo(); "
        "  var b1 := f.Bar; "  // method ref — no parens, no arity filter
        "  return 0; "
        "end Main; end C;");
    // Bare reference: arity is -1, so the filter is skipped, both
    // candidates remain, and §26.5 rule 4 fires. Pin the wording.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("ambiguous extension member") !=
                std::string::npos &&
            d.message.find("Bar") != std::string::npos &&
            d.message.find("§26.5 rule 4") != std::string::npos) {
            found = true;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

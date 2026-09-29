// Spec-derived tests for issue #4735 — §28.3 + Appendix C.3 item 14, v0.307:
// the §28.3 must-use WARNING's EMITTED TEXT names the rule the spec uses for
// it. Per the spec verbatim, the §28.3 auto-drop "carries the must-use
// warning" (Appendix C.3 item 14, v0.307) and "the §28.3 must-use warning is
// emitted at the implicit-drop site" (§28.9, v0.307). The implementation had
// been emitting "is implicitly disposed at the end of its scope (§28.3
// auto-drop)" without ever naming "must-use" in the byte stream — a diagnostic
// that does not say "must-use" cannot be located from a user reading the
// warning, because the spec, the corpus case titles, the conformance rule
// column, and every reference in the rationale use that name.
//
// THE FIX. Append "must-use" to the parenthetical at every emission site in
// src/ownership/Ownership.cpp — both the OWN-LOCAL plain-`var` site (L1011,
// gated by `localIsTrivialDisposeNewBinding`) and the OWNED-PARAMETER twin
// (L1073, gated by `isNonTrivialDisposeClass` after Lane 24-A1's #4743
// wrap), plus the iter-var, frame-teardown, inline-local, and while-drain
// arms. The new wording reads "(§28.3 must-use / auto-drop)" — both names
// are in the spec, both stay in the byte stream; only the spec-canonical
// name is added.
//
// FAIL-WITHOUT. The pre-fix compiler emits "(§28.3 auto-drop)" with no
// "must-use" anywhere in the byte stream; the post-fix compiler emits
// "(§28.3 must-use / auto-drop)". The `find("must-use")` substring check
// below pin's the new wording precisely. It is not a vacuous oracle:
// deleting the change makes the substring searches return false on the
// same input, on the same compiler, with no other test difference.
//
// SCOPE. This test pins the wording at the two emission sites Lane 24-A1
// was already working at (L1011 OWN-LOCAL twin, L1073 OWNED-PARAMETER
// twin). The other emission sites — iter-var L772/L909/L1282, frame
// teardown L820/L862/L1323, and inline-local L1041/L1205/L1379/L1399 —
// carry the same change as part of the same code-touching commit (the spec
// alignment is uniform across holder forms; C.3 item 14 names both
// holder forms in one breath).
//
// The conformance runner's `// CHECK-SAME` lit tests pin a §28.3 fragment
// ("(§28.3 auto-drop)") and were updated to "(§28.3 must-use / auto-drop)"
// in this commit. The corpus's `//! diagnostic: must use` rule column is
// not gated by `verdict_for` for warn/reject cases (see
// `tests/conformance/rules/runner.py` L121 — `WARNING_RE.search` only);
// runner sweep under the fix reports 75 ok / 9 known-divergent / 0
// REGRESSION / 0 UNBANKED on §28.3 + §28.9 (matches pre-fix exactly; the
// 9 known-divergent are #4605/#4400/#4393, unrelated to this fix).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

namespace {

// Predicate: true when any warning emitted by the ownership pass contains
// the `must-use` substring. The §28.3 must-use warning is the rule the
// spec names; L1011 / L1073 / iter-var / frame-teardown / inline-local
// sites all carry the same compound name in the byte stream after the
// fix. Returning false pre-fix and true post-fix is what makes this a
// FAIL-WITHOUT test, not a vacuous oracle.
inline bool warningContainsMustUse(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("must-use") != std::string::npos) return true;
    }
    return false;
}

// §28.3 + Appendix C.3 item 14, v0.307 (L1011 OWN-LOCAL site). An
// undischarged plain-`var` owned local with a non-trivial-dispose type
// (the `Logger` class declares a destructor — §28.9's first disqualifier,
// mirrored from `tests/ownership/owned_param_must_use_4743_test.cpp`).
// The warning fires (L1011's `localIsTrivialDisposeNewBinding` predicate
// returns false for this shape) and the byte stream must now name
// "must-use" per the spec's own term.
const char* kOwnedLocalProgram =
    "module M; "
    "public class Logger "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Logger; "
    "class C public static method Main(): int "
    "  var l := new Logger(); "
    "  return 0; "
    "end Main; end C;";

// §28.3 + Appendix C.3 item 14, v0.307 (L1073 OWNED-PARAMETER site,
// post-#4743 trivial-dispose gate). An undischarged `owned` parameter
// of non-trivial-dispose type — the warning survives the A1 wrap
// (`isNonTrivialDisposeClass` returns true for `Logger`) and the byte
// stream at L1078 must now name "must-use".
const char* kOwnedParamProgram =
    "module M; "
    "public class Logger "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Stamp(): int => 7; "
    "end Logger; "
    "class C public static method Take(owned l: Logger): int "
    "  return l.Stamp(); "
    "end Take; end C;";

// §29.2 / §28.3 + Appendix C.3 item 14, v0.307 (L909 iter-var site, the
// inner `for var item` loop arm). The owned iteration variable `b` is
// undischarged at body end — auto-drop fires at body end with the
// per-iteration must-use warning, and the byte stream at L910 must now
// name "must-use". The shape mirrors `s28_3_0002` (the §28.3 corpus row
// that already carries `//! diagnostic: must use` in its rule column).
const char* kIterVarProgram =
    "module M; "
    "public class Box "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class Src public method Gen(): IOwningEnumerable<Box> "
    "  yield return new Box(); "
    "end Gen; end Src; "
    "class C public static method Main(): int "
    "  for var b in new Src().Gen() do end; "
    "  return 0; "
    "end Main; end C;";

}  // namespace

// §28.3 + Appendix C.3 item 14, v0.307 / fix #4735: the OWN-LOCAL twin at
// L1011 emits the §28.3 must-use warning at a plain-`var` owned local of
// non-trivial-dispose type. The byte stream must contain "must-use" so a
// reader can locate the spec rule from the warning text.
BEZEL_TEST(must_use_warning_text_4735,
           own_local_warning_names_must_use_spec_28_3_c3_item_14) {
    auto r = bezel::test::parseAndCheckOwnership(kOwnedLocalProgram);
    // The warning MUST fire for this shape (L1011's gate passes, no
    // `using var`, no explicit Dispose, no move). Pre-fix the warning
    // fires without naming "must-use"; post-fix it carries that name.
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "l"));
    BEZEL_EXPECT_TRUE(warningContainsMustUse(r));
}

// §28.3 + Appendix C.3 item 14, v0.307 / fix #4735: the OWNED-PARAMETER
// site at L1077 emits the §28.3 must-use warning for an undischarged
// `owned` parameter of non-trivial-dispose type. The byte stream must
// contain "must-use" — same alignment at the second emission site.
BEZEL_TEST(must_use_warning_text_4735,
           owned_param_warning_names_must_use_spec_28_3_c3_item_14) {
    auto r = bezel::test::parseAndCheckOwnership(kOwnedParamProgram);
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "l"));
    BEZEL_EXPECT_TRUE(warningContainsMustUse(r));
}

// §29.2 / §28.3 + Appendix C.3 item 14, v0.307 / fix #4735: the
// iter-var site at L909 emits the §28.3 must-use warning for an
// undischarged `for var b in …` of non-trivial-dispose element type.
// This is the third holder form the spec names in one breath (the
// iter-var; C.3 item 14 covers "every holder form the warning
// attaches to").
BEZEL_TEST(must_use_warning_text_4735,
           iter_var_warning_names_must_use_spec_29_2_28_3_c3_item_14) {
    auto r = bezel::test::parseAndCheckOwnership(kIterVarProgram);
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "b"));
    BEZEL_EXPECT_TRUE(warningContainsMustUse(r));
}

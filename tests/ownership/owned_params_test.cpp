// Spec-derived tests for Ownership slice O10 — owned parameter discharge
// (§22.3 + §28.3 + §28.8).
//
// An `owned` parameter is in the Owned state on method entry. It must
// reach a discharge point on every normal-exit path (same rule as a
// plain `var x := producer-of-owned;`). `out owned` follows the same
// rule on the callee side.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class T end T; class Resource end Resource; "
        "class C ") + method + " end C;";
}

} // namespace

// §28.3 v0.152 (#1008 Part B, extended to parameters): an undischarged `owned`
// parameter is no longer an error — it auto-drops at method end (the same
// implicit disposal the exception edge already performed) with the must-use
// warning.
BEZEL_TEST(own_param, owned_param_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "r"));
}

BEZEL_TEST(own_param, owned_param_explicitly_disposed_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) r.Dispose(); end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The return type is `owned Resource`, not `Resource` — REQUIRED as of v0.307
// and load-bearing for this row. §28.3's announcement table row 4 admits only
// a RECEIVER-SOURCED BORROW through a plain concrete class return, and an
// `owned` parameter is neither receiver-sourced nor a borrow: returning it is
// a transfer, which is spelled `owned`. The diagnostic says so itself
// ("declare the return type `owned` to transfer ownership to the caller").
//
// The row originally spelled the plain return and was admitted pre-v0.307.
// `fe95b173` implemented row 4 and it became a compile error. The row's axis
// — that an owned parameter may be discharged by returning it — is unchanged
// and still pinned; only the spelling of the transfer moved.
// per §28.3 announcement table row 4, §28.8, v0.307
BEZEL_TEST(own_param, owned_param_returned_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Pass(owned r: Resource): owned Resource "
        "  return r; "
        "end Pass;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_param, borrowed_param_no_obligation) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Inspect(r: Resource) end Inspect;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.2: a double Dispose() on an owned param is a WARNING (idempotent no-op),
// not an error — the second call is a guaranteed runtime no-op.
BEZEL_TEST(own_param, owned_param_double_disposed_warned_not_errored) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) "
        "  r.Dispose(); r.Dispose(); "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("disposed again") != std::string::npos)
            warned = true;
    BEZEL_EXPECT_TRUE(warned);
}

BEZEL_TEST(own_param, owned_param_moved_then_read_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) "
        "  _ := move r; "
        "  _ := r; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 v0.152: `r1` is explicitly disposed; the undischarged `r2` auto-drops at
// method end with the must-use warning — no error.
BEZEL_TEST(own_param, two_owned_params_one_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r1: Resource, owned r2: Resource) "
        "  r1.Dispose(); "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "r2"));
}

// (The dispose-exactly-once / no-double-free behaviour of the parameter
// auto-drop — on both the normal and exception edges, with a caller-`move`d
// source not double-freed — is verified in the codegen battery
// move_result_owned_724: owned_param_auto_drop_disposes_once.)

// ============================================================================
// fix #3037 / §28.3 (v0.240): the §28.3 owned-parameter auto-drop WARNING must
// be reported at the parameter declaration, NOT at 0:0. An `owned` parameter's
// disposal obligation is established at the parameter's own declaration (the
// parameter *is* an owned slot from method entry, not from an expression the
// tracker recorded), so `LocalOwnership::obligationOrigin` was always null for
// the parameter arm and the diagnostic fell through to `SourceLocation{}` —
// printing `0:0`, pointing at nothing. An IDE/editor integration consuming the
// diagnostic stream could not place the warning; a user with several `owned`
// parameters across a file was told only the parameter's name.
//
// The sibling LOCAL-variable auto-drop warning a few lines up in the same
// function is located correctly (`obligationOrigin` is the LocalDecl), so
// this is specific to the parameter arm.
//
// The fix: SymbolCollection::collectParams stamps `ParameterSymbol::declLoc`
// from `ast::Param::loc` (which the parser already populates at the
// parameter's first token — `Param` is not an `ast::Node`, so it carries a
// single `SourceLocation`, not a `span`), and the diagnostic site at
// Ownership.cpp's parameter arm falls back to `declLoc` when no
// obligationOrigin is recorded. Mirrors fix #2418 (FieldSymbol/MethodSymbol/
// etc.) and fix #2482 (PositionalField via FieldSymbol::declLoc).
// ============================================================================

namespace {

// Locate the §28.3 owned-parameter auto-drop warning in `r`'s diagnostics for
// parameter `name`. Returns the diagnostic's SourceLocation on match, or a
// sentinel {0,0,0} when not found. Filters out the local-form warning
// (`"end of its scope"`) so the helper is parameter-specific.
struct OwnedParamDiagLoc {
    bool found = false;
    bezel::SourceLocation loc{};
};

OwnedParamDiagLoc findOwnedParamDiag(const bezel::test::OwnershipResult& r,
                                     const std::string& name) {
    OwnedParamDiagLoc result;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        // The parameter arm says "implicitly disposed at the end of the
        // method" / "by the frame at teardown"; the local arm says "end of
        // its scope". Both contain the parameter name when the parameter is
        // shadowed by a local — pick the one whose message is unambiguous.
        // We use a param-only substring that the local arm does not contain.
        if (d.message.find("end of the method") == std::string::npos &&
            d.message.find("at teardown") == std::string::npos) continue;
        if (!result.found) {
            result.found = true;
            result.loc = d.location;
        }
    }
    return result;
}

}  // namespace

// The §28.3 sync non-iterator parameter auto-drop warning points at the
// parameter declaration, not at 0:0. Source uses a known column where the
// parameter `r` is declared.
BEZEL_TEST(own_param_3037, sync_param_auto_drop_warning_at_decl_not_zero) {
    const std::string src =
        "module M;\n"
        "class Resource end Resource;\n"
        "class C\n"
        "  public method Take(owned r: Resource)\n"
        "  end Take;\n"
        "end C;\n";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto info = findOwnedParamDiag(r, "r");
    BEZEL_EXPECT_TRUE(info.found);
    // The diagnostic MUST NOT be at 0:0 — the bug under test.
    BEZEL_EXPECT_TRUE(info.loc.line != 0);
    BEZEL_EXPECT_TRUE(info.loc.column != 0);
    // Pin the precise location: line 4 (the method-declaration line), at the
    // column where the parser stamped the parameter's first token. Indent
    // is two spaces, then "public method Take(" is 21 chars, so the `owned`
    // modifier that begins the parameter sits at column 22. Anything else is
    // a regression of #3037.
    BEZEL_EXPECT_EQ(info.loc.line, 4u);
    BEZEL_EXPECT_EQ(info.loc.column, 22u);
}

// Two `owned` parameters — the FIRST one discharged (no warning), the SECOND
// one undischarged (warning at the SECOND parameter's column, not 0:0, and
// not at the first parameter's column).
BEZEL_TEST(own_param_3037, second_owned_param_auto_drop_at_its_own_decl) {
    const std::string src =
        "module M;\n"
        "class Resource end Resource;\n"
        "class C\n"
        "  public method Take(owned r1: Resource, owned r2: Resource)\n"
        "    r1.Dispose();\n"
        "  end Take;\n"
        "end C;\n";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto info = findOwnedParamDiag(r, "r2");
    BEZEL_EXPECT_TRUE(info.found);
    BEZEL_EXPECT_TRUE(info.loc.line != 0);
    BEZEL_EXPECT_TRUE(info.loc.column != 0);
    // `r1` is disposed; only `r2` should draw the warning.
    auto info1 = findOwnedParamDiag(r, "r1");
    BEZEL_EXPECT_FALSE(info1.found);
    // Pin to the line/column of `r2`'s modifier. Line 4, after the `, ` and
    // space — count from 1: 2 indent + "public method Take(owned r1: "
    // (33 chars) = column 35 for `,`, column 36 for ` `, column 37 for the
    // start of `owned` — but the parser stamps `Param::loc` at the
    // parameter's first token, which is the modifier. The observed column
    // is 42 (verified at compile time; pinned to lock #3037 against any
    // parser-side loc-stamp change).
    BEZEL_EXPECT_EQ(info.loc.line, 4u);
    BEZEL_EXPECT_EQ(info.loc.column, 42u);
}

// Iterator / async iterator / plain async `owned` parameter whose obligation
// the frame disposes at teardown (§29.8 / §30.1) — same parameter-decl
// location rule, the `obligationDiagLoc` helper is shared with the sync
// arm. Verify the location is the parameter's column, not 0:0.
BEZEL_TEST(own_param_3037, iterator_param_auto_drop_warning_at_decl_not_zero) {
    const std::string src =
        "module M;\n"
        "class R end R;\n"
        "class C\n"
        "  public static method Gen(owned a: R): IOwningEnumerable<int>\n"
        "    yield return 0;\n"
        "  end Gen;\n"
        "end C;\n";
    auto r = parseAndCheckOwnership(src);
    // The "T must be class" error fires first because int is a value type;
    // suppress by checking only Warning-severity diagnostics.
    auto info = findOwnedParamDiag(r, "a");
    BEZEL_EXPECT_TRUE(info.found);
    BEZEL_EXPECT_TRUE(info.loc.line != 0);
    BEZEL_EXPECT_TRUE(info.loc.column != 0);
    BEZEL_EXPECT_EQ(info.loc.line, 4u);
    // "  public static method Gen(" is 27 chars; "owned" starts at column 28.
    BEZEL_EXPECT_EQ(info.loc.column, 28u);
}

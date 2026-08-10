// Spec-derived parser tests for the post-colon `owned` parameter modifier
// — issue #3027, §15.4.1, Appendix A §A.4, v0.240.
//
// A.4 declares the Param grammar as:
//   Param ::= ParamModifiers? Identifier ":" Type (":=" Expression)?
//   ParamModifiers ::= "ref" | "out" "owned"? | "owned" | "scoped"
//
// The owning-form `FromCollectionLiteral` example in §15.4.1 writes the
// modifier AFTER the colon:
//   public static method FromCollectionLiteral(
//       elements: owned OwningArray<T>): owned C<T>;
//
// Pre-fix, this rejected with "expected type name" at the `owned` token
// because the Param production admits ParamModifiers only BEFORE the
// identifier and `parseType` does not accept `owned` as a named-type
// prefix. The fix relaxes Param to:
//   Param ::= ParamModifiers? Identifier ":" ("owned")? Type
// mirroring the `owned?` already admitted on return types (MethodDecl /
// OperatorDecl / IfaceMethod, A.4 grammar lines ~1042/1037/10299).
// Post-colon `owned` is semantically identical to modifier-before-name
// `owned`: both produce `modifier=="owned"` on the parsed Param node,
// and the existing modifier=="owned" sema path handles ownership
// transfer uniformly.
//
// Tests:
//   1. POSITIVE — §15.4.1 spec example: `Identifier ":" owned Type`.
//   2. POSITIVE — modifier-before-name form still parses (regression).
//   3. POSITIVE — bare-move-only form still parses (regression).
//   4. POSITIVE — post-colon `owned` with a default value.
//   5. NEGATIVE — `ref` + post-colon `owned` is rejected (modifier not
//      empty; `owned` after `:` falls through to `parseType` which
//      rejects it as a type name).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

bool succeedsParse(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

} // namespace

// §15.4.1 owning-form spec example: `Identifier ":" "owned" Type`.
BEZEL_TEST(parser_param_post_colon_owned_3027,
           owning_form_spec_example_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public class C<T> "
        "  public static method FromCollectionLiteral("
        "    elements: owned OwningArray<T>"
        "  ): owned C<T>; "
        "end C;"));
}

// Modifier-before-name form (the A.4-grammar-strict spelling) still parses.
BEZEL_TEST(parser_param_post_colon_owned_3027,
           owning_form_modifier_before_name_still_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public class C<T> "
        "  public static method FromCollectionLiteral("
        "    owned elements: OwningArray<T>"
        "  ): owned C<T>; "
        "end C;"));
}

// Bare-move-only parameter (no `owned` at all) still parses — the existing
// move-only-by-value semantics suffice for OwningArray<T> parameters per
// §28.3. Verifying the fix did not regress the no-modifier path.
BEZEL_TEST(parser_param_post_colon_owned_3027,
           owning_form_bare_move_only_still_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public class C<T> "
        "  public static method FromCollectionLiteral("
        "    elements: OwningArray<T>"
        "  ): owned C<T>; "
        "end C;"));
}

// Post-colon `owned` combined with a default value (the `":="` follows the
// Type per A.4; `owned` lives strictly between `:` and `Type`).
BEZEL_TEST(parser_param_post_colon_owned_3027,
           post_colon_owned_with_default) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public class C "
        "  public method M(x: owned int := 0): int => x; "
        "end C;"));
}

// Negative: `ref x: owned T` — the parameter already carries a non-empty
// modifier, so the post-colon `owned` falls through to `parseType()` which
// rejects it (`owned` is not a type name). The fix must NOT relax this
// boundary; a `ref` parameter does not take ownership and a stray
// post-colon `owned` would silently miscompile.
BEZEL_TEST(parser_param_post_colon_owned_3027,
           ref_with_post_colon_owned_rejected) {
    auto r = parseAll(
        "module M; "
        "public class C "
        "  public method M(ref x: owned int): int => x; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
// Regression test for #6434: isGeneric inferred from key presence.
//
// THE BUG. resolveNamed's generic-param check used s->names.count(first) — key
// presence only. When a LocalTypeSymbol (e.g. Box) was registered with a
// nullptr value in typeScope.names, count() returned 1, incorrectly marking it
// as a generic parameter. A local type nested inside another local type would
// then receive: "local type references the enclosing generic parameter `Box`"
// — even though the program declares NO generic parameters.
//
// THE FIX. Two parts:
//   1. In resolveNamed: changed from count() to checking for the specific
//      nullptr value that marks generic-param markers.
//   2. In resolveTypeDecl and resolveTypesInStmt: removed the nullptr fallback
//      that registered LocalTypeSymbol names as nullptr markers, which collided
//      with the generic-param sentinel.
//
// PRE-FIX BEHAVIOUR (r30-c1, measured). The r1 probe from the r31-rv-localtype
// review:
//
//   module M; import System;
//   public class Outer
//     public method M(): int
//       record Inner
//         public record Deep
//           public record Box(W: int);
//           public method Get(): Box return new Box(7); end Get;
//         end Deep;
//       end Inner;
//       var d := new Inner.Deep(); return d.Get().W;
//     end M;
//   end Outer;
//
// Declares NO generic parameters. Pre-fix: rejected with
// "local type references the enclosing generic parameter `Box`" — the bug.
//
// POST-FIX BEHAVIOUR. The same program is still rejected (with
// "unresolved type name `Box`"), because local-type-to-local-type resolution
// by bare name is not yet implemented. That is a separate, pre-existing
// defect. This test pins the REMOVED diagnostic as absent.
//
// CITATION. §21.10 v0.334: "A local type may not reference an enclosing
// generic parameter (an enclosing method's or class's `T`) — a compile error
// naming the rule." The bug reported this for a program with no generic
// parameters at all.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The spurious diagnostic #6434 produced before the fix. This phrase must NOT
// appear in the output for a program that declares no generic parameters.
constexpr const char* kSpuriousGenericParamDiagnostic =
    "references the enclosing generic parameter";

// The diagnostic that survives the fix — a separate, pre-existing defect
// where local types nested inside other local types are not resolved by bare
// name. This is NOT the bug being fixed here; it is documented as the
// remaining issue.
constexpr const char* kSurvivingDiagnostic = "unresolved type name";

}  // namespace

// ── The #6434 shape: local type with no enclosing generic parameters ──
//
// The program declares NO generic parameters. Pre-fix: wrongly rejected with
// "local type references the enclosing generic parameter `Box`". Post-fix:
// still rejected, but with "unresolved type name `Box`" instead — a different
// defect that exists independently of #6434.
//
// This test asserts the BUG IS GONE (kSpuriousGenericParamDiagnostic absent)
// without asserting the survivor is correct (it is not — local-type-to-local-type
// resolution by bare name is a pre-existing gap).
BEZEL_TEST(local_type_isgeneric_false_positive_6434,
           no_generic_param_cannot_be_claimed_as_enclosing_generic) {
    // The r1 probe from the r31-rv-localtype review. No generic parameters.
    const std::string src =
        "module M; import System; "
        "public class Outer "
        "  public method M(): int "
        "    record Inner "
        "      public record Deep "
        "        public record Box(W: int); "
        "        public method Get(): Box return new Box(7); end Get; "
        "      end Deep; "
        "    end Inner; "
        "    var d := new Inner.Deep(); return d.Get().W; "
        "  end M; "
        "end Outer;";
    auto r = runAot(src);

    // The program should still be rejected — the survivor is a separate defect.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));

    // THE BUG IS GONE: the spurious generic-parameter diagnostic must not appear.
    // Pre-fix: this message was present. Post-fix: absent.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kSpuriousGenericParamDiagnostic));

    // The surviving diagnostic is "unresolved type name Box" — a separate bug where
    // local types nested inside other local types aren't resolved by bare name.
    // This is NOT what #6434 fixes; it exists independently and is documented
    // as a pre-existing gap.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSurvivingDiagnostic));
}

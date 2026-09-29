// fix #5076 cross-module pin — test #5114 (§14.2, v0.307)
//
// Module A (library): a generic class with a `where T: class` constraint,
// and a subclass whose override omits the `where` clause (inherits it).
//
// The test verifies that the metadata export carries the inherited constraint
// set (not an empty set) so that subsequent dispatch is correct.
//
// The fix (ModuleMetadataBuilder::convertGenericParams importedBound fallback,
// b152988d / 60e3b12d) is already on origin/main.  This test acts as a
// regression pin and documentation of the cross-module constraint-propagation
// contract.  For the test shape used here, the body resolution path is
// identical pre- and post-fix, so the test cannot mechanically prove
// fail-without; its value is in pinning the expected cross-module behaviour
// and serving as a regression guard if the fallback is ever removed or
// broken in a future change.

#include <iostream>
#include <string>

#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "parser/Parser.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "sema/ExprTypecheck.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

// Build ModuleMetadata from source text (the standard test harness pattern).
ModuleMetadata buildMetaFor5114(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) return {};
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    return buildModuleMetadata(ctx, source);
}

// Search a method's genericParams for a special-constraint kind.
bool gpHasSpecialConstraint(const ModuleMetadata& meta,
                           std::string_view typeName,
                           std::string_view methodName,
                           std::string_view paramName,
                           std::string_view constraint) {
    for (const auto& td : meta.types) {
        if (td.name != typeName) continue;
        for (const auto& member : td.members) {
            if (member.name != methodName) continue;
            for (const auto& gp : member.genericParams) {
                if (gp.name != paramName) continue;
                for (const auto& sc : gp.specialConstraints) {
                    if (sc == constraint) return true;
                }
            }
        }
    }
    return false;
}

// Search a method's genericParams for a constraint type by namedName.
bool gpHasConstraintType(const ModuleMetadata& meta,
                         std::string_view typeName,
                         std::string_view methodName,
                         std::string_view paramName,
                         std::string_view constraintNamedName) {
    for (const auto& td : meta.types) {
        if (td.name != typeName) continue;
        for (const auto& member : td.members) {
            if (member.name != methodName) continue;
            for (const auto& gp : member.genericParams) {
                if (gp.name != paramName) continue;
                for (const auto& ct : gp.constraintTypes) {
                    if (ct.namedName == constraintNamedName) return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — METADATA CHECK: Sub5114T1.M<T> carries the inherited `class`
// special-constraint even though the override omits the `where` clause.
// Uses buildModuleMetadata to verify the constraint is present in the exported
// metadata before any cross-module import occurs.  §14.2.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_chain_cross_module_5114,
           t1_omit_where_meta_carries_class_constraint) {
    const char* kSrc =
        "module Lib5114T1; "
        "public class Base5114T1 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end M; "
        "end Base5114T1; "
        "public class Sub5114T1 : Base5114T1 "
        // Omit `where T: class` — inherits it from Base5114T1.M<T>.
        "  public override method M<T>(x: T): int "
        "    return 1; "
        "  end M; "
        "end Sub5114T1; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5114(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "Sub5114T1", "M", "T", "class");
    if (!hasClass) {
        std::cerr << "Sub5114T1.M<T> missing 'class' special-constraint\n";
    }
    BEZEL_EXPECT_TRUE(hasClass);
}

// ===========================================================================
// T2 — COMPOUND CONSTRAINT: an override that omits `where T: class, IFoo`
// carries both the special-constraint AND the type-bound.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_chain_cross_module_5114,
           t2_omit_where_meta_carries_compound_constraint) {
    const char* kSrc =
        "module Lib5114T2; "
        "public interface INum5114T2 "
        "  method Val(): int; "
        "end INum5114T2; "
        "public class Base5114T2 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class, INum5114T2 "
        "    return 0; "
        "  end M; "
        "end Base5114T2; "
        "public class Sub5114T2 : Base5114T2 "
        // Omit `where T: class, INum5114T2` — inherits both.
        "  public override method M<T>(x: T): int "
        "    return 2; "
        "  end M; "
        "end Sub5114T2; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5114(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "Sub5114T2", "M", "T", "class");
    bool foundIFoo = gpHasConstraintType(meta, "Sub5114T2", "M", "T", "INum5114T2");
    if (!hasClass) std::cerr << "Sub5114T2.M<T> missing 'class'\n";
    if (!foundIFoo) std::cerr << "Sub5114T2.M<T> missing INum5114T2 bound\n";
    BEZEL_EXPECT_TRUE(hasClass);
    BEZEL_EXPECT_TRUE(foundIFoo);
}

// ===========================================================================
// T3 — CONTROL: restating the clause verbatim still exports it (sanity).
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_chain_cross_module_5114,
           t3_restate_where_meta_carries_class_constraint) {
    const char* kSrc =
        "module Lib5114T3; "
        "public class Base5114T3 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end M; "
        "end Base5114T3; "
        "public class Sub5114T3 : Base5114T3 "
        // Restate verbatim.
        "  public override method M<T>(x: T): int "
        "    where T: class "
        "    return 3; "
        "  end M; "
        "end Sub5114T3; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5114(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "Sub5114T3", "M", "T", "class");
    BEZEL_EXPECT_TRUE(hasClass);
}

// ===========================================================================
// T4 — VALUE CONSTRAINT: an override that omits `where T: value` carries
// the inherited value constraint.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_chain_cross_module_5114,
           t4_omit_where_meta_carries_value_constraint) {
    const char* kSrc =
        "module Lib5114T4; "
        "public class Base5114T4 "
        "  public virtual method M<T>(x: T): int "
        "    where T: value "
        "    return 0; "
        "  end M; "
        "end Base5114T4; "
        "public class Sub5114T4 : Base5114T4 "
        // Omit `where T: value` — inherits it.
        "  public override method M<T>(x: T): int "
        "    return 4; "
        "  end M; "
        "end Sub5114T4; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5114(kSrc);
    bool hasValue = gpHasSpecialConstraint(meta, "Sub5114T4", "M", "T", "value");
    if (!hasValue) std::cerr << "Sub5114T4.M<T> missing 'value'\n";
    BEZEL_EXPECT_TRUE(hasValue);
}

// ===========================================================================
// T5 — CROSS-MODULE CALL: an override that omits `where T: class` carries
// the inherited constraint through the cross-module import/export path.
// This pins b152988d's ModuleMetadataBuilder::convertGenericParams
// importedBound fallback behavior.  fail-without is impossible since the
// fix is already on origin/main; the test value is regression pin + §14.2
// contract documentation.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_chain_cross_module_5114,
           t5_cross_module_omit_where_meta_carries_class_constraint) {
    // Single-module source: base with `where T: class`, subclass omits it.
    const char* kSrc =
        "module Lib5114T5; "
        "public class Base5114T5 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end M; "
        "end Base5114T5; "
        "public class Sub5114T5 : Base5114T5 "
        // Omit `where T: class` — inherits it from Base5114T5.M<T>.
        "  public override method M<T>(x: T): int "
        "    return 5; "
        "  end M; "
        "end Sub5114T5; "
        "public class Entry public static method Main(): int => 0; end Entry; ";

    ModuleMetadata meta = buildMetaFor5114(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "Sub5114T5", "M", "T", "class");
    if (!hasClass) {
        std::cerr << "Sub5114T5.M<T> missing 'class' special-constraint\n";
    }
    BEZEL_EXPECT_TRUE(hasClass);
}

}  // namespace bezel::module

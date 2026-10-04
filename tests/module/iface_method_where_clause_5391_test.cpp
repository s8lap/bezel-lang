// fix #5391 (§14.2 / A.4, v0.314): interface generic methods can now
// declare `where` clauses. Two parts:
//
// 1. Parser (Parser_Decls.cpp): `parseInterfaceMember` now calls
//    `parseGenericConstraints()` after the return type (the same point
//    class methods call it at line 2578). Before this fix, `where`
//    on an interface method was a syntax error.
//
// 2. Sema (TypeResolution_Checks.cpp): `checkAbstractImplementations`
//    now checks that an implementing method's `where` clause matches
//    the interface method's constraint set (or is omitted to inherit
//    it). A non-equivalent clause is a compile error.
//
// Spec: §14.2 / A.4 (v0.314, #5248).

#include "support/aot_support.hpp"

#include <cstdio>
#include <string>

#include "test_harness.hpp"

namespace bezel::module {
namespace {

// ===========================================================================
// PART 1: PARSER — an interface method with a `where` clause compiles.
// ===========================================================================

BEZEL_TEST(iface_method_where_clause_5391,
           interface_method_with_where_clause_compiles) {
    // The literal repro from the issue: a `where` clause on an interface
    // generic method was a syntax error before the parser fix.
    const char* kSrc =
        "module M; "
        "public interface I "
        "  method M<T>(x: T): int where T: class; "
        "end I; "
        "public class C : I "
        "  public constructor() end constructor; "
        "  public method M<T>(x: T): int return 42; end M; "
        "end C; "
        "public class Entry public static method Main(): int => 0; end Entry;";
    auto r = test::runAot(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// PART 2: SEMA — an implementation with a DIFFERENT `where` clause is rejected.
// ===========================================================================

BEZEL_TEST(iface_method_where_clause_5391,
           impl_with_non_matching_where_clause_rejected) {
    // Interface declares `where T: class`; implementation declares
    // `where T: value`. These are not equivalent, so the sema check
    // should report an error.
    const char* kSrc =
        "module M; "
        "public interface I "
        "  method M<T>(x: T): int where T: class; "
        "end I; "
        "public class C : I "
        "  public constructor() end constructor; "
        "  public method M<T>(x: T): int "
        "    where T: value "
        "    return 42; "
        "  end M; "
        "end C; "
        "public class Entry public static method Main(): int => 0; end Entry;";
    auto r = test::runAot(kSrc);
    // Should fail to compile (did not run).
    BEZEL_EXPECT_FALSE(r.ran);
    // Should have a diagnostic about the `where` clause mismatch.
    bool hasWhereDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("where") != std::string::npos ||
            d.message.find("equivalent") != std::string::npos) {
            hasWhereDiag = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasWhereDiag);
}

// ===========================================================================
// PART 3: SEMA — an implementation that omits the `where` clause is fine
// (inherits the interface's constraints).
// ===========================================================================

BEZEL_TEST(iface_method_where_clause_5391,
           impl_omits_where_inherits_interface_constraint) {
    // Interface declares `where T: class`; implementation omits the clause
    // (inherits it). Should compile successfully — the omitted clause means
    // "exactly the inherited set" per §14.2.
    const char* kSrc =
        "module M; "
        "public interface I "
        "  method M<T>(x: T): int where T: class; "
        "end I; "
        "public class C : I "
        "  public constructor() end constructor; "
        "  public method M<T>(x: T): int return 42; end M; "
        "end C; "
        "public class Entry public static method Main(): int => 0; end Entry;";
    auto r = test::runAot(kSrc);
    // Should compile (ran = true) even though implementation omits the clause.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace
}  // namespace bezel::module

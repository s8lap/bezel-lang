// Regression tests for issue #4755 — §9.7 (v0.307): records and enums can
// never implement `IDisposable`/`IAsyncDisposable` (only classes implement them,
// implicitly per stdlib §28.2). The builtin interfaces resolve to a Named
// TypeRef WITHOUT a synthesized declaration, so without the fix the call-site
// constraint checker fell through the type-parameter-bound branch (which defers
// on builtin interfaces) to the decl-null defer below, silently accepting the
// invalid instantiation.
//
// THE DEFECT. A record (or enum) passed as a type argument to a generic
// constrained with `where T: IDisposable` was SILENTLY ACCEPTED at the method-
// call site (ExprTypecheck_Calls.cpp) and at the type-instantiation site
// (TypeResolution_Checks.cpp).
//
// THE FIX. In ExprTypecheck_Calls.cpp, two guards handle the builtin
// interfaces (IDisposable/IAsyncDisposable) at the call-site constraint
// enforcer:
//   1. The existing primitive-vs-IDisposable guard handled primitives.
//   2. A new Named/Record-or-Enum guard catches the record/enum cases.
//      When the argument TypeRef is Named, its declaration is a Record or Enum,
//      and the bound is a builtin interface without a declaration, report the
//      constraint violation.
//
// DIAGNOSTIC FORMAT (per ExprTypecheck_Calls.cpp:4664-4679):
//   "type argument `XXX` does not satisfy the constraint `where T: YYY` (§14.2)"
//
// Authority: spec §9.7 (v0.307), stdlib §28.2.
//
// SCOPE. Covers both halves of the fix:
//   * record type argument rejected (call site AND type instantiation)
//   * enum type argument rejected (call site AND type instantiation)
// And one control:
//   * class implicitly implementing IDisposable still accepted
// (Primitive behavior is already pinned by primitive_builtin_iface_generic_arg_1867.)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// A generic static method whose T is constrained with `where T: IDisposable`.
// Called at the method-call constraint-check site.
//
// `typeArg` is the expression to pass as the type argument
// (e.g. "new Money(5)" or "Color.Red").
std::string callSiteIDisposable(std::string_view typeArg) {
    std::string prog = "module M; ";
    prog += "public record Money(Amount: int); ";
    prog += "public enum Color ";
    prog += "  Red, Green, Blue; ";
    prog += "end Color; ";
    prog += "public static class Helpers ";
    prog += "  public static method Accepts<T>(x: T): bool ";
    prog += "  where T: IDisposable => true; ";
    prog += "end Helpers; ";
    prog += "class C ";
    prog += "  public static method Main() ";
    prog += "    Console.WriteLine(Helpers.Accepts(";
    prog += std::string(typeArg);
    prog += ")); ";
    prog += "  end Main; ";
    prog += "end C;";
    return prog;
}

// A generic record class whose T is constrained with `where T: IDisposable`.
// The instantiation of Box<T> exercises the type-instantiation constraint check.
//
// `typeName` is the type name to use for the type argument (e.g. "Money" or "Color").
std::string typeInstIDisposable(std::string_view typeName) {
    std::string prog = "module M; ";
    prog += "public record Money(Amount: int); ";
    prog += "public enum Color ";
    prog += "  Red, Green, Blue; ";
    prog += "end Color; ";
    prog += "public record Box<T>(V: T) where T: IDisposable; ";
    prog += "class C ";
    prog += "  public static method Main(): int ";
    if (typeName == "Money") {
        prog += "    var v := new Money(42); ";
    } else {
        prog += "    var v := Color.Red; ";
    }
    prog += "    var b := new Box<";
    prog += std::string(typeName);
    prog += ">(v); ";
    prog += "    return 0; ";
    prog += "  end Main; ";
    prog += "end C;";
    return prog;
}

// Helper: returns true iff `r` was rejected AND produced the specific
// constraint-not-satisfied diagnostic for IDisposable, naming `typeArgName`.
// Diagnostic format: `type argument `XXX` does not satisfy the constraint
// `where T: IDisposable` (§14.2)` — backtick-enclosed.
bool rejectedByIDisposableConstraint(const bezel::test::JitResult& r,
                                    std::string_view typeArgName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("does not satisfy the constraint") ==
            std::string::npos)
            continue;
        if (d.message.find("IDisposable") == std::string::npos) continue;
        if (d.message.find(typeArgName) != std::string::npos) return true;
    }
    return false;
}

// Helper: returns true iff `r` was accepted AND produced NO diagnostic
// containing "does not satisfy the constraint" with "IDisposable".
// Used as a negative guard on the accept control: if a future change
// makes the compiler reject everything (including valid classes), this catches it.
bool acceptedWithNoIDisposableConstraintError(
    const bezel::test::JitResult& r) {
    if (!r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("does not satisfy the constraint") !=
            std::string::npos &&
            d.message.find("IDisposable") != std::string::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ══════════════ REJECT rows (fix-critical) ═══════════════════════════════════════

// §9.7 (v0.307): a record type argument at a `where T: IDisposable` constraint
// must be rejected at the call site with the specific constraint-not-satisfied
// diagnostic (not a parse error, not a missing method, etc.).
BEZEL_TEST(record_idisposable_constraint_4755,
           callsite_record_Money_vs_idisposable_rejected) {
    // l2_10_0004: the conformance case that was silently accepted before the fix.
    // Exact diagnostic: `type argument `Money` does not satisfy the constraint
    // `where T: IDisposable` (§14.2)`
    auto r = runAot(callSiteIDisposable("new Money(5)"));
    BEZEL_EXPECT_TRUE(rejectedByIDisposableConstraint(r, "Money"));
}

// §9.7 (v0.307): same rejection at the type-instantiation site
// (`new Box<Money>(...)`).
BEZEL_TEST(record_idisposable_constraint_4755,
           typeinst_record_Money_vs_idisposable_rejected) {
    // Exact diagnostic: `type argument `Money` does not satisfy the constraint
    // `where T: IDisposable` (§14.2)`
    auto r = runAot(typeInstIDisposable("Money"));
    BEZEL_EXPECT_TRUE(rejectedByIDisposableConstraint(r, "Money"));
}

// §9.7 (v0.307): an enum type argument at a `where T: IDisposable` constraint
// must be rejected at the call site (the fix handles both Record AND Enum).
BEZEL_TEST(record_idisposable_constraint_4755,
           callsite_enum_Color_vs_idisposable_rejected) {
    // Exact diagnostic: `type argument `Color` does not satisfy the constraint
    // `where T: IDisposable` (§14.2)`
    auto r = runAot(callSiteIDisposable("Color.Red"));
    BEZEL_EXPECT_TRUE(rejectedByIDisposableConstraint(r, "Color"));
}

// §9.7 (v0.307): same rejection at the type-instantiation site.
BEZEL_TEST(record_idisposable_constraint_4755,
           typeinst_enum_Color_vs_idisposable_rejected) {
    // Exact diagnostic: `type argument `Color` does not satisfy the constraint
    // `where T: IDisposable` (§14.2)`
    auto r = runAot(typeInstIDisposable("Color"));
    BEZEL_EXPECT_TRUE(rejectedByIDisposableConstraint(r, "Color"));
}

// ══════════════ ACCEPT controls (fix must not break these) ════════════════════════

// §9.7 / stdlib §28.2: a CLASS is the only valid substitution for
// `where T: IDisposable`. Every class implicitly implements IDisposable
// (§28.2), so a plain class is accepted. Uses runAot to exercise the
// full end-to-end path. Negative guard: no constraint-not-satisfied
// diagnostic with "IDisposable" in the message.
BEZEL_TEST(record_idisposable_constraint_4755,
           class_implicitly_idisposable_still_accepted) {
    // Based on l2_10_0003.bzl (stdlib §2.10, "admits every class implicitly").
    auto r = runAot(R"BZ(
module M;
public class Resource
  public constructor() end constructor;
end Resource;
public static class Helpers
  public static method Accepts<T>(x: T): bool where T: IDisposable => true;
end Helpers;
class C
  public static method Main()
    Console.WriteLine(Helpers.Accepts(new Resource()));
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
    // Negative guard: no constraint-not-satisfied diagnostic.
    BEZEL_EXPECT_TRUE(acceptedWithNoIDisposableConstraintError(r));
}

// Spec-derived tests for issue #7665 — a method reference over a USER's own
// extension member on a built-in collection (`Array<T>`) must form a
// function value.
//
// THE DEFECT. `ExprTypecheck_Calls.cpp`'s §27.6 bare-reference reject for the
// built-in collection heads (`Array<T>` / `OwningArray<T>`, stdlib §4.4 /
// §4.5) fires when the referenced name is one of the head's own DECLARED
// methods, one of the universal `Object` members (§8.1), or a stdlib §5 LINQ
// operator name — with the stated reason that "this builtin collection
// dispatches it structurally rather than through a real method symbol, so no
// function value can be formed for it yet".
//
// That reason is FALSE for a USER extension member. A member declared in a
// user's `extension Array<int>` block IS a real method with a real
// `MethodSymbol`: it is found by the §26.5 rule-2 extension lookup, and
// codegen dispatches it through the extension's own symbol — NOT through the
// collection's structural dispatch. §27.6 (v0.355) is explicit: "For an
// extension method, the reference is formed in either of the two spellings a
// call admits. ... **Receiver-qualified** — `v.Scaled` over `extension Vec`,
// the member resolved on the value exactly as the call `v.Scaled()` is
// (§26.5) — the reference is a bound reference". §26.5 says the same of
// every member access: "The outcome is the same for every member access on a
// value, not only a call — a property or field read or write, and a method
// reference, which resolves its member exactly as the call does (§27.6)".
//
// So the user's own `extension Array<int> { method Sum(): int }` member is
// reachable by BOTH spellings, and the bare reference must be admitted.
//
// THE FIX. The §26.5 rule-2 extension lookup now runs BEFORE that reject,
// and the reject is skipped when the lookup finds the USER's own member.
//
// WHAT MUST NOT CHANGE (the MUST-STAY-GREEN rows):
//   * a bare reference to a stdlib §5 LINQ operator (`Where` / `Select` /
//     `Take` / `First` / ...) on `Array<T>` — those operators are the
//     compiler-known §5 block's members, dispatched structurally, so no
//     function value can be formed for them (issue #2492);
//   * a bare reference to a universal `Object` member (`ToString` /
//     `Equals` / ...) on `Array<T>` (issue #2470);
//   * a bare reference to a user extension over a USER class (unchanged);
//   * the CALL form of every one of those.
//
// per §26.5 rule 2 / §26.5.1 / §27.6 (v0.355), stdlib §4.4 / §4.5 / §5,
// §8.1, issue #7665.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"

namespace {

using bezel::ast::MemberAccessExpr;
using bezel::ast::MethodDecl;
using bezel::ast::TypeDecl;
using bezel::sema::MethodSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// The §27.6 bare-reference reject's own text — a reject that fired for
// another reason does not carry it.
bool rejectsAsBareStructuralOperator(const bezel::test::SemaResult& r) {
    return r.diagnostics.hasErrors() &&
           anyDiagContains(r, "27.6") &&
           anyDiagContains(r, "not yet supported on");
}

// The first member access whose member name is `member`, anywhere in the
// module's type declarations.
const MemberAccessExpr* findMemberAccess(const bezel::test::SemaResult& r,
                                         std::string_view member) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md = dynamic_cast<const MethodDecl*>(m.get());
            if (!md) continue;
            if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
                md->exprBody) {
                if (const auto* ma =
                        dynamic_cast<const MemberAccessExpr*>(
                            md->exprBody.get())) {
                    if (ma->member == member) return ma;
                }
            }
            if (md->bodyKind != bezel::ast::MethodBodyKind::Block) continue;
            for (const auto& s : md->stmts) {
                if (!s) continue;
                if (const auto* ma =
                        dynamic_cast<const MemberAccessExpr*>(s.get())) {
                    if (ma->member == member) return ma;
                }
                // A local declaration's initializer:
                // `var f: () -> int := a.Sum;`
                const auto* ld =
                    dynamic_cast<const bezel::ast::LocalDecl*>(s.get());
                if (!ld || !ld->initializer) continue;
                const auto* ima =
                    dynamic_cast<const MemberAccessExpr*>(
                        ld->initializer.get());
                if (ima && ima->member == member) return ima;
            }
        }
    }
    return nullptr;
}

// The USER's own extension member over `Array<int>`, named `Sum` — the
// issue's own repro shape.
constexpr const char* kUserArrayExt =
    "extension Array<int> public method Sum(): int => 77; end; ";

// A USER extension over a USER class — the control that isolates the defect
// to the built-in-collection receiver.
constexpr const char* kUserClassExt =
    "class MyClass public method N(): int => 5; end MyClass; "
    "extension MyClass public method F(): int => 42; end; ";

}  // namespace

// ── The defect: the user's own extension member over `Array<T>` ─────────────

// §27.6 / §26.5: the receiver-qualified extension reference resolves the
// member exactly as the call does, and forms a BOUND function value — the
// receiver is captured, so the formed type is the method's own parameters
// only (`() -> int`).
BEZEL_TEST(sema_ext_methodref_array_7665, user_extension_memberref_forms_function) {
    auto r = parseSemaAll(
        std::string("module R; ") + kUserArrayExt +
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f: () -> int := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "not yet supported on"));
    auto* ma = findMemberAccess(r, "Sum");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        // The member is a REAL method with a real MethodSymbol — the
        // extension's own, not a structural dispatch.
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Function);
        if (t && t->kind == TypeRef::Kind::Function) {
            // A BOUND reference: the receiver is captured, so the formed
            // type carries the method's own parameters only.
            BEZEL_EXPECT_TRUE(t->funcParams.empty());
            BEZEL_EXPECT_TRUE(t->funcReturn &&
                              t->funcReturn->primitive == PrimitiveType::Int);
        }
    }
}

// §26.5 rule 2 / §26.5.1: the head must MATCH the receiver's element type —
// an `extension Array<string>` member is reachable from an `Array<string>`.
BEZEL_TEST(sema_ext_methodref_array_7665, user_extension_head_matches_receiver) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<string> public method Sum(): int => 77; end; "
        "class Program public static method Main(): int "
        "  var a: Array<string> := [\"a\",\"b\"]; "
        "  var f: () -> int := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "not yet supported on"));
}

// §26.5.1: the head must match the RECEIVER's element type — an
// `extension Array<int>` member is NOT a candidate on an `Array<string>`
// receiver, so the bare reference names no member and is rejected.
BEZEL_TEST(sema_ext_methodref_array_7665, user_extension_head_mismatch_rejected) {
    auto r = parseSemaAll(
        std::string("module R; ") + kUserArrayExt +
        "class Program public static method Main(): int "
        "  var a: Array<string> := [\"a\",\"b\"]; "
        "  var f: () -> int := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// §27.6 (v0.307): the OVERLOADED method-group rule — the reference is
// resolved against the TARGET function type, so `() -> int` selects the
// no-argument form out of the overloaded set.
BEZEL_TEST(sema_ext_methodref_array_7665, overloaded_memberref_selects_by_target_type) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<int> "
        "  public method Sum(): int => 77; "
        "  public method Sum(x: int): int => x; "
        "end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f: () -> int := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findMemberAccess(r, "Sum");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Function);
        if (t && t->kind == TypeRef::Kind::Function) {
            BEZEL_EXPECT_TRUE(t->funcParams.empty());
        }
    }
}

// §27.6 (v0.307): with NO target type, an overloaded method group is
// ambiguous — the §27.6 diagnostic, not the §27.6 bare-reference reject.
BEZEL_TEST(sema_ext_methodref_array_7665, overloaded_memberref_without_target_is_ambiguous) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<int> "
        "  public method Sum(): int => 77; "
        "  public method Sum(x: int): int => x; "
        "end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "overloaded"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "27.6"));
}

// §7.4: a NULLABLE receiver is rejected by the nullability rule, which is
// read BEFORE the member is resolved — unchanged by this fix.
BEZEL_TEST(sema_ext_methodref_array_7665, nullable_receiver_rejected_by_7_4) {
    auto r = parseSemaAll(
        std::string("module R; ") + kUserArrayExt +
        "class Program public static method Main(): int "
        "  var a: Array<int>? := [1,2,3]; "
        "  var f: () -> int := a.Sum; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "7.4"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "not yet supported on"));
}

// §26.5 rule 2: a user extension whose name is NOT a §5 operator name was
// ALREADY reachable by a bare reference (the §26.5 rule-2 fallback) — the
// control that isolates the defect to the §5-NAMED surface.
BEZEL_TEST(sema_ext_methodref_array_7665, non_linq_named_user_extension_unchanged) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<int> public method Total(): int => 77; end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f: () -> int := a.Total; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findMemberAccess(r, "Total");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Function);
    }
}

// §26.5 rule 1: a name the receiver's OWN declaration commits (stdlib
// §4.4's `Array<T>.Add`) is NEVER resolved through the extension blocks —
// the bare reference stays refused.
BEZEL_TEST(sema_ext_methodref_array_7665, rule1_committed_name_still_refused) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<int> public method Add(): int => 77; end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f: () -> int := a.Add; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// §26.5 rule 1 / §8.1: a name the `Object` base commits (`ToString`) is
// NEVER resolved through the extension blocks — the bare reference stays
// refused.
BEZEL_TEST(sema_ext_methodref_array_7665, rule1_committed_object_name_still_refused) {
    auto r = parseSemaAll(
        "module R; "
        "extension Array<int> public method ToString(): string => \"x\"; end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f: () -> string := a.ToString; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// ── MUST-STAY-GREEN: the stdlib §5 LINQ operator names ─────────────────────

// §5 / #2492: `Where` is a stdlib §5 LINQ operator — dispatched
// STRUCTURALLY by codegen (no real method symbol), so no function value can
// be formed for it. STAYS REFUSED.
BEZEL_TEST(sema_ext_methodref_array_7665, stdlib_linq_operator_still_refused) {
    auto r = parseSemaAll(
        "module R; class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f := a.Where; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// §5 / #2492: the other §5 names the same way.
BEZEL_TEST(sema_ext_methodref_array_7665, other_stdlib_linq_operators_still_refused) {
    for (const char* name : {"Select", "Take", "First"}) {
        auto r = parseSemaAll(
            std::string("module R; class Program public static method Main(): int "
                        "  var a: Array<int> := [1,2,3]; "
                        "  var f := a.") +
            name + "; return 0; end Main; end Program;");
        BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
    }
}

// ── MUST-STAY-GREEN: the universal `Object` members ────────────────────────

// §8.1 / #2470: `ToString` is a universal `Object` member — dispatched
// structurally for this receiver, so no function value can be formed for it.
// STAYS REFUSED.
BEZEL_TEST(sema_ext_methodref_array_7665, universal_tostring_still_refused) {
    auto r = parseSemaAll(
        "module R; class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f := a.ToString; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// §8.1 / #2470: `Equals` likewise.
BEZEL_TEST(sema_ext_methodref_array_7665, universal_equals_still_refused) {
    auto r = parseSemaAll(
        "module R; class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var f := a.Equals; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(rejectsAsBareStructuralOperator(r));
}

// ── MUST-STAY-GREEN: the CALL forms ───────────────────────────────────────

// §26.5: the CALL `a.Sum()` over the user's own extension is unchanged —
// it resolved through the very same §26.5 rule-2 lookup, and still does.
BEZEL_TEST(sema_ext_methodref_array_7665, call_form_over_user_extension_unchanged) {
    auto r = parseSemaAll(
        std::string("module R; ") + kUserArrayExt +
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var n := a.Sum(); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5 / #2492: the CALL form of a stdlib §5 operator is unaffected.
BEZEL_TEST(sema_ext_methodref_array_7665, stdlib_linq_call_form_unchanged) {
    auto r = parseSemaAll(
        "module R; class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var b := a.Where(lambda(x: int) => x > 2); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── MUST-STAY-GREEN: a USER extension over a USER class ────────────────────

// §26.5 rule 2 / §27.6: the control that isolates the defect to the
// built-in-collection receiver — a user extension over a user class already
// formed a function value, and still does.
BEZEL_TEST(sema_ext_methodref_array_7665, user_class_extension_memberref_unchanged) {
    auto r = parseSemaAll(
        std::string("module R; ") + kUserClassExt +
        "class Program public static method Main(): int "
        "  var m: MyClass := new MyClass(); "
        "  var f: () -> int := m.F; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* ma = findMemberAccess(r, "F");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Function);
    }
}

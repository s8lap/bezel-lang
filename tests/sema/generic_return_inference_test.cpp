// Sema — return-target generic type-argument inference (issue #391).
//
// A generic method whose type parameter appears ONLY in the return type —
// `Create<T>(): T` — cannot have T inferred from arguments (no argument names
// T). Bezel has no call-site type-argument syntax: §A.7 PrimarySuffix has no
// `<...>` invocation form, and `Create<Widget>()` parses as a relational
// comparison chain. So the only way to call such a method is to let T be
// inferred from the *target* type at the call site.
//
// Spec basis. §14.1 declares generic methods (`method Swap<T>(...)`); §14.2
// declares their constraints. Neither states the inference algorithm. §26.5
// (extension-member resolution: "The inference follows the same unification
// rules as generic method type inference") confirms that the mechanism is
// unification. The "target type" concept the inference unifies against is the
// same one the spec already uses for collection literals (§15.4: "well-typed
// only in a context that provides the target type ... via type annotation ...
// `return [1,2,3]` is admitted when the enclosing method's return type
// determines the target type") and for the `new()` shorthand (§17.7: permitted
// in "typed local-variable declarations, return statements (when the method has
// a declared return type)" but "not permitted ... [for] `var` without
// annotation").
//
// DECLINE-WITH-REASONS. The spec does not state how a generic method's
// return-only type parameter is solved. This milestone implements the
// conservative interpretation: unify the method's declared return type against
// the target type to solve any type parameter the arguments left unbound,
// scoped to the two unambiguous target contexts the spec already recognizes —
// (a) a typed local declaration initialized by the call, and (b) a `return`
// statement when the enclosing method has a declared return type. Contexts
// with no determinable target (a bare expression statement, `var w := ...`
// with no annotation, an argument position) get a "cannot infer" diagnostic,
// matching the spec's treatment of an undeterminable collection-literal target.
//
// These tests are load-bearing: before the implementation the typed-target
// cases left the call typed as the bare unresolved `T` (so a trailing member
// access did not resolve and no assignability check fired), and the no-target
// cases were silently accepted. After the implementation, the typed-target
// call types as the concrete target and the no-target cases diagnose.

#include <functional>
#include <memory>
#include <vector>

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some emitted diagnostic mentions the return-target inference
// failure ("cannot infer the type argument"). Used so the no-target tests
// assert that *this* check fired.
bool hasInferDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot infer the type argument") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) return true;
    }
    return false;
}

// Find the first CallExpr that is the initializer of a LocalDecl named
// `localName`, anywhere in the source. Returns null if none.
const bezel::ast::CallExpr* localInitCall(const bezel::test::SemaResult& r,
                                          std::string_view localName) {
    const bezel::ast::CallExpr* found = nullptr;
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->bodyKind != bezel::ast::MethodBodyKind::Block)
                continue;
            for (const auto& s : md->stmts) {
                auto* ld =
                    dynamic_cast<const bezel::ast::LocalDecl*>(s.get());
                if (ld && ld->name == localName && ld->initializer) {
                    if (auto* ce = dynamic_cast<const bezel::ast::CallExpr*>(
                            ld->initializer.get())) {
                        found = ce;
                        return found;
                    }
                }
            }
        }
    }
    return found;
}

// Find the first CallExpr that is the value of a `return` statement, anywhere
// in the source. Returns null if none.
const bezel::ast::CallExpr* returnValueCall(const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->bodyKind != bezel::ast::MethodBodyKind::Block)
                continue;
            for (const auto& s : md->stmts) {
                auto* rs =
                    dynamic_cast<const bezel::ast::ReturnStmt*>(s.get());
                if (rs && rs->value) {
                    if (auto* ce = dynamic_cast<const bezel::ast::CallExpr*>(
                            rs->value.get()))
                        return ce;
                }
            }
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) typed local declaration — the call types as the declared local type
// ---------------------------------------------------------------------------

// §14.1 / §15.4 (target type via annotation): `var w: Widget := Factory.Create();`
// — T appears only in `Create<T>(): T`, so it is solved from the target
// `Widget`. The call's result type must be the concrete `Widget`, not the
// bare unresolved `T`.
// Before: the call was typed as the unresolved `T` (Named, no declaration).
BEZEL_TEST(sema_generic_return_infer_391, typed_local_solves_return_only_param) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Create<T>(): T where T: class, new() "
        "    return new T(); "
        "  end Create; "
        "end Factory; "
        "class C public method M() "
        "  var w: Widget := Factory.Create(); "
        "end M; end C;");
    auto* call = localInitCall(r, "w");
    BEZEL_EXPECT_TRUE(call != nullptr);
    if (call) {
        auto* t = r.ctx->exprType(*call);
        BEZEL_EXPECT_TRUE(t != nullptr);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
        BEZEL_EXPECT_TRUE(t && t->declaration != nullptr &&
                          t->declaration->name == "Widget");
    }
    // Solving the target means no spurious assignability error.
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.1: the inference still works when the target is a different class —
// `Make<T>(): T` selected by the target `Gadget`.
BEZEL_TEST(sema_generic_return_infer_391, typed_local_other_target_type) {
    auto r = parseSemaAll(
        "module M; "
        "class Gadget public constructor() end constructor; end Gadget; "
        "static class Factory "
        "  public static method Make<T>(): T where T: class, new() "
        "    return new T(); "
        "  end Make; "
        "end Factory; "
        "class C public method M() "
        "  var g: Gadget := Factory.Make(); "
        "end M; end C;");
    auto* call = localInitCall(r, "g");
    BEZEL_EXPECT_TRUE(call != nullptr);
    if (call) {
        auto* t = r.ctx->exprType(*call);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named &&
                          t->declaration && t->declaration->name == "Gadget");
    }
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (b) return statement — the call types as the enclosing return type
// ---------------------------------------------------------------------------

// §15.4 / §17.7 (target type via `return`): a method returning `Widget` whose
// body is `return Factory.Create();` solves T = Widget from the enclosing
// return type. After inference the returned value is `Widget`, so the §5.1
// return-assignability check passes (no error).
// Before: the returned value was the unresolved `T`; checkAssignable silently
// skipped it (no declaration), so the call was never solved.
BEZEL_TEST(sema_generic_return_infer_391, return_target_solves_return_only_param) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Create<T>(): T where T: class, new() "
        "    return new T(); "
        "  end Create; "
        "end Factory; "
        "class C "
        "  public method GetWidget(): Widget "
        "    return Factory.Create(); "
        "  end GetWidget; "
        "end C;");
    auto* call = returnValueCall(r);
    BEZEL_EXPECT_TRUE(call != nullptr);
    if (call) {
        auto* t = r.ctx->exprType(*call);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named &&
                          t->declaration && t->declaration->name == "Widget");
    }
    BEZEL_EXPECT_FALSE(hasInferDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// no-target contexts — a clear diagnostic, never silent / never a crash
// ---------------------------------------------------------------------------

// `var w := Factory.Create();` (no annotation) gives the call no target, so T
// is undeterminable. Per the spec's treatment of an undeterminable target
// (§15.4 "ERROR: cannot infer target type"), this must diagnose, not silently
// type the local as the unresolved `T`.
// Before: silently accepted.
BEZEL_TEST(sema_generic_return_infer_391, untyped_local_no_target_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Create<T>(): T where T: class, new() "
        "    return new T(); "
        "  end Create; "
        "end Factory; "
        "class C public method M() "
        "  var w := Factory.Create(); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasInferDiag(r));
}

// A bare expression statement `_ := Factory.Create();` discards the result, so
// there is no target. Diagnose rather than silently leaving T unsolved.
// Before: silently accepted.
BEZEL_TEST(sema_generic_return_infer_391, bare_call_statement_no_target_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Create<T>(): T where T: class, new() "
        "    return new T(); "
        "  end Create; "
        "end Factory; "
        "class C public method M() "
        "  _ := Factory.Create(); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasInferDiag(r));
}

// ---------------------------------------------------------------------------
// guard: argument-inferred generic methods are unaffected
// ---------------------------------------------------------------------------

// §14.1: when T is bound by an argument (`Clone<T>(proto: T): T`), the
// return-target path must NOT fire — the argument already solves T. The call
// types from the argument as before, with no inference diagnostic and no
// dependence on a target type (here the local is untyped).
BEZEL_TEST(sema_generic_return_infer_391, argument_inferred_param_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Clone<T>(proto: T): T where T: class, new() "
        "    return new T(); "
        "  end Clone; "
        "end Factory; "
        "class C public method M() "
        "  using var p := new Widget(); "
        "  var c := Factory.Clone(p); "
        "end M; end C;");
    // T is solved from the argument `p: Widget`; no target needed, no diag.
    BEZEL_EXPECT_FALSE(hasInferDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// guard: a non-generic method's typed-local call is untouched — the
// return-target path only engages for generic methods with an unbound param.
BEZEL_TEST(sema_generic_return_infer_391, non_generic_call_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "static class Factory "
        "  public static method Get(): Widget return new Widget(); end Get; "
        "end Factory; "
        "class C public method M() "
        "  var w: Widget := Factory.Get(); "
        "end M; end C;");
    auto* call = localInitCall(r, "w");
    BEZEL_EXPECT_TRUE(call != nullptr);
    if (call) {
        auto* t = r.ctx->exprType(*call);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named &&
                          t->declaration && t->declaration->name == "Widget");
    }
    BEZEL_EXPECT_FALSE(hasInferDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

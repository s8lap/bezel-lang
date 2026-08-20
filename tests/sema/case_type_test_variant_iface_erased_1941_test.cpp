// Spec-derived tests for §19.3/§12.7/§2.8.3 rule 4 (#1941, spec v0.220): the erasure
// guard on a VARIANT (`in`/`out`) generic-interface scrutinee.
//
// §12.7: "`is T` tests whether a value is an instance of `T`" — and §19.3's
// typed binding narrows to exactly the pattern's instantiation. #1927/#1946
// closed the INVARIANT-interface-scrutinee erasure hole via
// `typeTestInterfaceBindAmbiguous`, but deliberately held the check OFF a
// variant scrutinee interface ("a distinct erasure slice needing variance
// reasoning" — the recorded residual #1941). §12.9: a variant parameter admits
// a subtype/supertype RANGE of conforming class instantiations. Post-#1843 the
// runtime test is PRECISE when every pattern type argument is runtime-reified,
// so the variant hold-off is harmless there. But for a still-ERASED argument
// (a function type — appendDescKey collapses Kind::Function to "F", §14.2's
// monomorphization carries no identity token for it) the descriptor collapses
// every sibling instantiation, and the variant range guarantees siblings are
// admissible — so the arm binds a `C<Q≠A>` value at `C<A>`: the same
// memory-unsafe type confusion class as #2001/#2006.
//
// Verified live on HEAD dd33abd (probe /var/tmp/bezel-scratch/laneE/
// p1941_erased.bzl): a `Prod<(string)->string>` behind a covariant
// `ISrc<(string)->string>` scrutinee compiled and BOUND at
// `Prod<(int)->int>` (exit 1). The reified shape (p1941_reified.bzl,
// `Box<Dog>` at `IRead<Animal>`) does NOT mis-bind at runtime (exit 0), so it
// must STAY accepted — no over-rejection (pins #1927's
// covariant_scrutinee_residual_accepted / #1920's covariant accept).
//
// The fix: in `typeTestInterfaceBindAmbiguous`, a variant scrutinee interface
// with an all-reified pattern stays admitted (runtime-precise), while an
// erased-argument pattern is rejected as ErasedAmbiguous unless the arm is
// provably dead (a sealed pattern class that implements the scrutinee
// interface nowhere).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The #1946-worded ERASED-ambiguous reason (shared by this guard): "not
// runtime-distinguishable" appears in no other diagnostic of this family.
bool hasErasedAmbiguousError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- REJECTED: variant scrutinee + still-erased pattern argument -------------

// (1) The verified-live hole (statement-form case): covariant `ISrc<out T>`,
// `Prod<T>: ISrc<T>`, scrutinee `ISrc<(string)->string>`, arm
// `var x: Prod<(int)->int>`. The erased `is Prod` test matches every `Prod<Q>`
// and the covariant range admits siblings, so a `Prod<(string)->string>` value
// binds at `Prod<(int)->int>` (probe: exit 1). REJECTED (FAILS WITHOUT the
// fix: the variant hold-off admitted it).
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           covariant_erased_function_arg_rejected_19_3) {
    auto r = parseSemaAll(wrap(
        "interface ISrc<out T>\nend ISrc;\n"
        "class Prod<T> : ISrc<T>\nend Prod;\n",
        "  public method M(s: ISrc<(string) -> string>): int\n"
        "    case s of\n"
        "      var x: Prod<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (2) CONTRAVARIANT scrutinee: `ISink<in T>` — the same reasoning through the
// supertype direction of §12.9's range. REJECTED.
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           contravariant_erased_function_arg_rejected_12_9) {
    auto r = parseSemaAll(wrap(
        "interface ISink<in T>\nend ISink;\n"
        "class Cons<T> : ISink<T>\nend Cons;\n",
        "  public method M(s: ISink<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: Cons<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (3) MIXED variance, erased argument in the INVARIANT slot: `IBi<T, out U>`,
// pattern `Duo<(int)->int, Foo>`. Any variant parameter on the scrutinee
// interface disables the invariant uniqueness reasoning, and the function-typed
// argument is erased — the descriptor collapses, siblings admissible through
// the `out U` range. REJECTED.
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           mixed_variance_erased_arg_rejected_12_9) {
    auto r = parseSemaAll(wrap(
        "interface IBi<T, out U>\nend IBi;\n"
        "class Foo\nend Foo;\n"
        "class Duo<T, U> : IBi<T, U>\nend Duo;\n",
        "  public method M(s: IBi<(int) -> int, Foo>): int\n"
        "    case s of\n"
        "      var x: Duo<(int) -> int, Foo> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (4) VALUE-form case (CaseExpr) — the same guard runs on the expression form
// (both forms call typeTestInterfaceBindAmbiguous). REJECTED.
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           value_form_covariant_erased_rejected_19_3) {
    auto r = parseSemaAll(wrap(
        "interface ISrc<out T>\nend ISrc;\n"
        "class Prod<T> : ISrc<T>\nend Prod;\n",
        "  public method M(s: ISrc<(string) -> string>): int\n"
        "    var n := (case s of\n"
        "      var x: Prod<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// ---- ACCEPTED: no over-rejection of the runtime-precise / dead shapes --------

// (5) Covariant + all-REIFIED pattern (the #1927/#1920 accept pins' shape):
// `IRead<out T>`, scrutinee `IRead<Animal>`, arm `var x: Box<Dog>`. Post-#1843
// the runtime test is precise (probe p1941_reified: a `Box<Animal>` value does
// NOT mis-bind, exit 0), so the variant arm stays ACCEPTED — this pins that the
// #1941 fix rejects ONLY the erased regime.
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           covariant_reified_arg_stays_accepted_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IRead<out T>\nend IRead;\n"
        "class Animal\nend Animal;\n"
        "sealed class Dog : Animal\nend Dog;\n"
        "sealed class Box<T> : IRead<T>\nend Box;\n",
        "  public method M(b: IRead<Animal>): int\n"
        "    case b of\n"
        "      var x: Box<Dog> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) INVARIANT interface control: the variant guard must not touch the
// invariant domain — the erased-UNIQUE-conformer admit of #1927(B) on a SEALED
// class stays as it was (accepted by typeTestInterfaceBindAmbiguous itself;
// upstream #1913/#1920 conservatism aside, no ERASED-ambiguous wording may
// appear).
BEZEL_TEST(case_type_test_variant_iface_erased_1941,
           invariant_domain_untouched_no_erased_error_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    case m of\n"
        "      var x: C<int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
}

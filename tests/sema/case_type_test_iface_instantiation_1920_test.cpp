// Spec-derived tests for §19.3/§19.5/§5.1/§12.9 (#1920): the ADMISSION of a
// typed binding pattern `var x: C<A>` (a class pattern) against a generic
// INTERFACE scrutinee `I<B>`, when the pattern class's implemented
// instantiations of that interface are all incompatible with the scrutinee.
//
// This is the INTERFACE-scrutinee counterpart of #1913. A typed binding is a
// type-test-plus-bind that binds the matched value at the STATIC type `C<A>`;
// the runtime type-test erases generic arguments (it checks only the class
// tag). So `var x: Box<string>` against an `IBox<int>` scrutinee, where
// `Box<T>: IBox<T>`, would match an `IBox<int>` value at runtime (the erased
// test is just "is Box?") and bind it at `Box<string>` — a type-safety hole,
// since `Box<string>` implements only `IBox<string>`, which is disjoint from
// `IBox<int>` under invariance (§12.9). #1913 does not reach this: its
// class-chain projection returns null for an interface common ancestor
// (deliberately leaving the "implemented-twice" hazard alone).
//
// The fix reasons over the pattern class's FULL set of implemented interface
// instantiations, and rejects only for a FINAL pattern class (plain `sealed`,
// non-abstract — no subclass can add a compatible implementation, §12.4) whose
// entire implemented-interface set is disjoint from the scrutinee. The
// rejection must fire ONLY then: a genuinely-conforming instantiation, a class
// that implements the interface twice at DISTINCT instantiations (one of them
// compatible), a covariant match, a rigid scrutinee parameter, and a non-final
// pattern class (a subclass could add the implementation — conservative accept)
// must all stay ACCEPTED (no over-rejection).

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

// True iff an ERROR diagnostic mentions the #1920 wrong-interface-instantiation
// reason — pins THIS defect (its message names the "scrutinee interface type",
// the substring the #1913 class-scrutinee diagnostic does NOT carry).
bool hasIfaceInstantiationError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("scrutinee interface type") != std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- FAIL-without / PASS-with: incompatible interface instantiations REJECTED

// (1) The issue's repro, EXPRESSION position: scrutinee `IBox<int>`, arm
// `var x: Box<string>`; `Box<T>: IBox<T>` implements only `IBox<string>`, which
// is disjoint from `IBox<int>` under §12.9. FAILS WITHOUT the fix (#1913's
// class-chain projection is null for the interface common ancestor).
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           expr_wrong_iface_instantiation_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "sealed class Box<T> : IBox<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := case b of\n"
        "      var x: Box<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasIfaceInstantiationError(r));
}

// (2) Same repro, STATEMENT position (the second admission site — CaseStmt).
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           stmt_wrong_iface_instantiation_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "sealed class Box<T> : IBox<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := 0;\n"
        "    case b of\n"
        "      var x: Box<string> => n := 1,\n"
        "      _ => n := 2\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasIfaceInstantiationError(r));
}

// (3) INDIRECT implementation through a class base: `Box<T>: Mid<T>` and
// `Mid<T>: IBox<T>`. `Box<string>` still implements only `IBox<string>` (the
// walk composes the class base then the interface base). REJECTED.
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           indirect_iface_via_class_base_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "class Mid<T> : IBox<T>\nend Mid;\n"
        "sealed class Box<T> : Mid<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := case b of\n"
        "      var x: Box<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasIfaceInstantiationError(r));
}

// (4) NON-GENERIC pattern class at a wrong instantiation: `sealed class
// IntBox: IBox<int>` implements only `IBox<int>`; against an `IBox<string>`
// scrutinee it can never match. REJECTED (the pattern need not itself be
// generic — the mismatch is on the implemented interface).
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           non_generic_pattern_wrong_instantiation_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "sealed class IntBox : IBox<int>\nend IntBox;\n",
        "  public method M(b: IBox<string>): int\n"
        "    var n := case b of\n"
        "      var x: IntBox => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasIfaceInstantiationError(r));
}

// ---- No over-rejection: legitimate patterns stay ACCEPTED (controls) ----

// (5) GENUINELY-CONFORMING instantiation: scrutinee `IBox<int>`, arm
// `var x: Box<int>` — `Box<int>` implements `IBox<int>` exactly. Must compile
// clean.
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           conforming_instantiation_accepted) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "sealed class Box<T> : IBox<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := case b of\n"
        "      var x: Box<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasIfaceInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) IMPLEMENTED-TWICE (the hazard the class-case check deliberately avoids):
// `sealed class C<T>: IMarker<T>, IMarker<int>` implements the interface at TWO
// instantiations. Against an `IMarker<int>` scrutinee, arm `var x: C<string>`
// is admissible — `C<string>` implements `IMarker<int>` DIRECTLY, so a value of
// scrutinee type can be a `C<string>` and the bind is sound. Must stay ACCEPTED
// (the full implemented-set must be considered, not a single projection).
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           implemented_twice_reachable_instantiation_accepted) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: C<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasIfaceInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) COVARIANT interface: `IReadOnly<out T>`, scrutinee `IReadOnly<Animal>`,
// arm `var x: Box<Dog>`. `Box<Dog>` implements `IReadOnly<Dog>`, which
// reference-converts to `IReadOnly<Animal>` (Dog <: Animal, covariant §12.9), so
// a `Box<Dog>` value IS an `IReadOnly<Animal>`. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           covariant_interface_match_accepted) {
    auto r = parseSemaAll(wrap(
        "interface IReadOnly<out T>\nend IReadOnly;\n"
        "class Animal\nend Animal;\n"
        "sealed class Dog : Animal\nend Dog;\n"
        "sealed class Box<T> : IReadOnly<T>\nend Box;\n",
        "  public method M(b: IReadOnly<Animal>): int\n"
        "    var n := case b of\n"
        "      var x: Box<Dog> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasIfaceInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) NON-FINAL pattern class: a plain (non-sealed) `class Box<T>: IBox<T>`.
// Even at the disjoint instantiation `var x: Box<string>` @ `IBox<int>`, a
// subclass of `Box<string>` could add an `IBox<int>` implementation, so the
// pattern is NOT provably impossible. Conservative ACCEPT (the sound-subset
// boundary — the residual for the clash-reasoning extension).
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           non_final_pattern_class_not_over_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "class Box<T> : IBox<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := case b of\n"
        "      var x: Box<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasIfaceInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) RIGID scrutinee parameter: in a generic method the scrutinee is
// `IBox<U>` (U rigid). Arm `var x: Box<int>` is admissible at the instantiation
// U := int (a type pattern is tested per instantiation, §19.3 v0.182), so it
// must NOT be rejected at the declaration — variantGenericAssignable treats the
// rigid U as compatible. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_iface_instantiation_1920,
           rigid_scrutinee_param_not_over_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "sealed class Box<T> : IBox<T>\nend Box;\n",
        "  public method Rig<U>(b: IBox<U>): int\n"
        "    var n := case b of\n"
        "      var x: Box<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end Rig;\n"));
    BEZEL_EXPECT_FALSE(hasIfaceInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

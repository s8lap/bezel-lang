// Spec-derived tests for §19.3/§19.5/§5.1/§12.9 (#1927): the UNIQUENESS
// admission of a typed binding pattern `var x: C<A>` (a class pattern) against a
// generic INTERFACE scrutinee `I<B>`. #1920 (case_type_test_iface_instantiation_
// 1920_test.cpp) rejects only when NO instantiation of `C` conforms to `I<B>`
// (the arm is unreachable). #1927 is the DUAL hole: `C<A>` DOES conform, but is
// NOT the SOLE instantiation of `C` that does, so the erased runtime type-test
// (which checks only the class tag, §19.3) also matches a `C<Q≠A>` value and
// binds it at the wrong static type `C<A>`.
//
// Both residuals #1927 records are exploitable erasure holes (each reproduced as
// an AddressSanitizer heap-buffer-overflow before this fix):
//
//   (a) Non-final pattern class, `Box<T>: IBox<T>` at scrutinee `IBox<int>`,
//       pattern `var x: Box<string>`: a runtime `Box<int>` value (implements
//       `IBox<int>`, so it IS a value of scrutinee type) passes the erased
//       `is Box` test and binds at `Box<string>` — a wrong-type bind. #1920
//       admitted this because `Box` is non-final; but the `Box<int>` witness is
//       a SAME-class instantiation, so no subclass reasoning is needed — the
//       single implemented instantiation `IBox<T>` invariant-matches `IBox<int>`
//       pinning `T = int ≠ string`, a PROVABLE second conformer.
//
//   (b) Implemented-twice, `C<T>: IMarker<T>, IMarker<int>` at scrutinee
//       `IMarker<int>`, pattern `var x: C<int>`: a runtime `C<string>` value
//       implements `IMarker<int>` DIRECTLY (the ground clause), so every `C<Q>`
//       conforms; it passes `is C` and binds at `C<int>`. The ground implemented
//       instantiation leaves `C`'s parameter FREE — a provable second conformer.
//
// The check fires ONLY on a PROVABLE second conformer and stays INVARIANT-only:
// a variant (`in`/`out`) scrutinee interface admits a subtype range — a distinct
// erasure slice (a recorded residual, orthogonal to the variance checker) that
// must stay ACCEPTED here. A conforming unique instantiation, an
// implemented-twice shape where only ONE branch matches the scrutinee, a rigid
// scrutinee parameter, and the covariant residual must all stay ACCEPTED (no
// over-rejection).

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

// True iff an ERROR diagnostic carries the #1927 uniqueness reason — pins THIS
// defect: its message names "not determined by the scrutinee", which the #1920
// reachability diagnostic ("no implemented instantiation is compatible") does
// NOT carry.
bool hasBindAmbiguousError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not determined by the scrutinee") !=
                std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- FAIL-without / PASS-with: the undetermined-argument bind is REJECTED ----

// (a) EXPRESSION position: non-final `Box<T>: IBox<T>` at scrutinee `IBox<int>`,
// arm `var x: Box<string>`. `Box<int>` conforms to `IBox<int>` and int ≠ string,
// so the erased `is Box` test would bind a `Box<int>` value at `Box<string>`.
// REJECTED (FAILS WITHOUT the fix — #1920 admits the non-final case).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           expr_nonfinal_distinct_pin_rejected) {
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
    BEZEL_EXPECT_TRUE(hasBindAmbiguousError(r));
}

// (a) STATEMENT position (the second admission site — CaseStmt).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           stmt_nonfinal_distinct_pin_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IBox<T>\nend IBox;\n"
        "class Box<T> : IBox<T>\nend Box;\n",
        "  public method M(b: IBox<int>): int\n"
        "    var n := 0;\n"
        "    case b of\n"
        "      var x: Box<string> => n := 1,\n"
        "      _ => n := 2\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasBindAmbiguousError(r));
}

// (b) EXPRESSION position: FINAL `C<T>: IMarker<T>, IMarker<int>` at scrutinee
// `IMarker<int>`, arm `var x: C<int>`. The ground clause `IMarker<int>` makes
// EVERY `C<Q>` conform, so a `C<string>` value would bind at `C<int>`. REJECTED
// (FAILS WITHOUT the fix — #1920 admits it: SOME implemented instantiation is
// compatible, which is reachability, not uniqueness).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           expr_implemented_twice_ground_free_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: C<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasBindAmbiguousError(r));
}

// (b) STATEMENT position.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           stmt_implemented_twice_ground_free_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := 0;\n"
        "    case m of\n"
        "      var x: C<int> => n := 1,\n"
        "      _ => n := 2\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasBindAmbiguousError(r));
}

// (b, variant) An EXTRA class parameter the scrutinee does not mention:
// `Pair<T, U>: IMarker<T>` at scrutinee `IMarker<int>`, arm `var x: Pair<int,
// string>`. `IMarker<T>` pins `T = int` but leaves `U` FREE — a `Pair<int, bool>`
// value conforms and would bind at `Pair<int, string>`. REJECTED.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           free_extra_class_parameter_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class Pair<T, U> : IMarker<T>\nend Pair;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: Pair<int, string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasBindAmbiguousError(r));
}

// ---- No over-rejection: legitimately-unique / undecidable binds stay ACCEPTED

// (1) The unique conforming instantiation: `Box<T>: IBox<T>` at scrutinee
// `IBox<int>`, arm `var x: Box<int>`. `Box<int>` is the ONLY `Box` instantiation
// implementing `IBox<int>` (invariant), so the bind is sound. Must compile clean.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           unique_conforming_instantiation_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (2) Implemented-twice where only ONE branch matches the scrutinee:
// `C<T>: IMarker<T>, IMarker<int>` at scrutinee `IMarker<string>`, arm
// `var x: C<string>`. Only `IMarker<T>` (pinning `T = string`) matches — the
// ground `IMarker<int>` does NOT match `IMarker<string>` — so `C<string>` is the
// unique conformer and the bind is sound. Must stay ACCEPTED (the uniqueness
// check must not blanket-reject the implemented-twice shape).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           implemented_twice_single_matching_branch_accepted) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<string>): int\n"
        "    var n := case m of\n"
        "      var x: C<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (3) RIGID scrutinee parameter: in a generic method the scrutinee is `IBox<U>`
// (U rigid). Arm `var x: Box<int>` is admissible at the instantiation U := int (a
// type pattern is tested per instantiation, §19.3 v0.182), so the uniqueness
// question is undecidable AT THE DECLARATION and must not be rejected. ACCEPTED.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           rigid_scrutinee_param_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (4) COVARIANT scrutinee interface (the recorded residual): `IReadOnly<out T>`,
// scrutinee `IReadOnly<Animal>`, arm `var x: Box<Dog>`. A variant parameter
// admits a subtype RANGE of conforming instantiations — a distinct erasure slice
// needing variance reasoning, left for a follow-up. The uniqueness check is
// INVARIANT-only, so this stays ACCEPTED (documents the deferred boundary; no
// over-rejection of the variant shape).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           covariant_scrutinee_residual_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

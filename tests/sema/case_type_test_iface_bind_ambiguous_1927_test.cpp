// Spec-derived tests for §19.3/§19.5/§5.1/§12.9 (#1927; narrowed by #1946): the
// DEAD-ARM admission of a typed binding pattern `var x: C<A>` (a class pattern)
// against a generic INTERFACE scrutinee `I<B>`. #1920 (case_type_test_iface_
// instantiation_1920_test.cpp) rejects an unreachable arm only for a FINAL
// pattern class; #1927 is the NON-FINAL / generic-class counterpart: it rejects
// `var x: C<A>` when the concrete `C<A>` provably CANNOT conform to `I<B>`, so no
// value of the scrutinee type is ever a `C<A>` and the arm can never match.
//
// #1946 REFINED this check into a THREE-WAY rule. As landed under #1927 it
// rejected EVERY non-unique conformer, on the premise that the runtime type-test
// ERASES generic arguments, so a `C<Q≠A>` value would bind at `C<A>`. #1843
// (per-monomorphization RTTI descriptors) made `is C<A>` PRECISE — BUT ONLY when
// every type argument of `A` is runtime-REIFIED (a concrete class/record/enum/
// primitive). For a still-erased #1939 kind (a function type, inline array) the
// descriptor collapses siblings and the test is NOT precise. So:
//
//   DEAD (REJECTED, "unreachable"): non-final `Box<T>: IBox<T>` at scrutinee
//       `IBox<int>`, pattern `var x: Box<string>`. `Box<string>` implements only
//       `IBox<string>` (invariant, §12.9), never `IBox<int>` — no value of the
//       scrutinee type is ever a `Box<string>`.
//
//   LIVE + REIFIED arg (ACCEPTED, was over-rejected): implemented-twice
//       `C<T>: IMarker<T>, IMarker<int>` at `IMarker<int>`, pattern `var x:
//       C<string>` / `C<int>`, and the free-extra-parameter shape with a concrete
//       argument. #1843 makes `is C<string>` precise, so only genuine `C<string>`
//       values match — LIVE and sound.
//
//   LIVE + ERASED arg (REJECTED, "not runtime-distinguishable"): the SAME shapes
//       but with a function-typed argument (`var x: C<(int)->int>`). #1843 does
//       NOT reify a function type, so `is C<(int)->int>` degrades and a sibling
//       `C<(string)->string>` mis-binds — the OLD #1927 rejection must stand.
//
// The check stays INVARIANT-only: a variant (`in`/`out`) scrutinee interface
// admits a subtype range — a distinct slice (a recorded residual, orthogonal to
// the variance checker) that stays ACCEPTED here. A conforming instantiation, a
// rigid scrutinee parameter, and the covariant residual all stay ACCEPTED.

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

// True iff an ERROR diagnostic carries the #1927 (narrowed #1946) dead-arm
// reason — pins THIS check: its message ends "the arm is unreachable", which the
// #1920 reachability diagnostic ("would bind at a wrong static type") does NOT
// carry, so the two remain distinguishable.
bool hasBindAmbiguousError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the arm is unreachable") != std::string::npos)
            return true;
    }
    return false;
}

// True iff an ERROR diagnostic carries the #1946 ERASED-ambiguous reason — a live
// arm whose differing type argument is a still-erased #1939 kind (function type,
// inline array) so the runtime test degrades and a sibling would mis-bind. Its
// message names "not runtime-distinguishable", carried by neither the #1920 nor
// the dead-arm diagnostic.
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

// ---- DEAD arms: an unreachable pattern instantiation is REJECTED -------------

// (a) EXPRESSION position: non-final `Box<T>: IBox<T>` at scrutinee `IBox<int>`,
// arm `var x: Box<string>`. `Box<string>` implements only `IBox<string>`
// (invariant, §12.9), never `IBox<int>`, so no value of the scrutinee type is
// ever a `Box<string>` — the arm is unreachable. REJECTED (FAILS WITHOUT the
// fix — #1920 admits the non-final case; #1946 keeps this dead-arm rejection).
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

// ---- LIVE arms (#1946): the pattern instantiation conforms — ACCEPTED --------

// (b) EXPRESSION position: FINAL `C<T>: IMarker<T>, IMarker<int>` at scrutinee
// `IMarker<int>`, arm `var x: C<int>`. `C<int>` conforms to `IMarker<int>` (via
// `T=int` AND the ground clause), so a real `C<int>` value flows in and — post-
// #1843 (precise `is C<int>`) — matches ONLY genuine `C<int>` values; no
// `C<Q≠int>` value ever binds here. The arm is LIVE and sound. Pre-#1946 #1927
// over-rejected this on the (now-false) erasure premise; #1946 admits it.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           expr_implemented_twice_ground_live_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b) STATEMENT position.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           stmt_implemented_twice_ground_live_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b, ground makes a DIFFERENT argument conform too) The #1946 headline case:
// `C<T>: IMarker<T>, IMarker<int>` at scrutinee `IMarker<int>`, arm
// `var x: C<string>`. `C<string>` conforms to `IMarker<int>` via the GROUND
// clause, so a `C<string>` value legitimately flows through the scrutinee and —
// post-#1843 — precisely matches `is C<string>`. LIVE and sound; ACCEPTED.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           expr_implemented_twice_ground_conformer_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b, variant) An EXTRA class parameter the scrutinee does not mention:
// `Pair<T, U>: IMarker<T>` at scrutinee `IMarker<int>`, arm `var x: Pair<int,
// string>`. `Pair<int, string>` implements `IMarker<int>` (`T=int`), so it
// conforms; post-#1843 `is Pair<int, string>` matches ONLY `Pair<int, string>`
// values (no `Pair<int, bool>` binds here). LIVE and sound; ACCEPTED (#1946).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           free_extra_class_parameter_live_accepted) {
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
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
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

// ---- ERASED-arg ambiguity (#1946): #1843 does NOT reify these — REJECTED ------
//
// #1843 makes `is C<A>` precise ONLY for a runtime-reified argument. A still-
// erased #1939 kind (a function type; the descriptor key collapses `Kind::Function`
// to "F" at any depth) makes the test degrade, so a conforming sibling `C<Q≠A>`
// mis-binds. The uniqueness rejection must STAND for these — this is exactly the
// blocker the pre-gate review of the first #1946 cut caught (a `C<(string)->string>`
// value wrongly matched `var x: C<(int)->int>`).

// (E1) IMPLEMENTED-TWICE with a FUNCTION-typed argument: `C<T>: IMarker<T>,
// IMarker<int>` at scrutinee `IMarker<int>`, arm `var x: C<(int) -> int>`. The
// ground `IMarker<int>` clause makes every `C<Q>` conform, and `(int)->int` is
// NOT runtime-reified, so `is C<(int)->int>` degrades and would bind a
// `C<(string)->string>` value at `C<(int)->int>`. REJECTED (the erased sibling).
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           implemented_twice_function_arg_erased_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: C<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
    // NOT the dead-arm diagnostic — this arm is LIVE (a `C<(int)->int>` conforms),
    // just not runtime-distinguishable from its sibling.
    BEZEL_EXPECT_FALSE(hasBindAmbiguousError(r));
}

// (E2) FREE extra parameter with a FUNCTION-typed argument: `Pair<T, U>:
// IMarker<T>` at scrutinee `IMarker<int>`, arm `var x: Pair<int, (int) -> int>`.
// `T` pins to `int` but `U` is free, and `U`'s pattern argument `(int)->int` is
// not reified, so a `Pair<int, (string)->string>` value would mis-bind. REJECTED.
BEZEL_TEST(case_type_test_iface_bind_ambiguous_1927,
           free_extra_function_arg_erased_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class Pair<T, U> : IMarker<T>\nend Pair;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: Pair<int, (int) -> int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// NOTE on the erased-UNIQUE-conformer case (`C<T>: IMarker<T>` at scrutinee
// `IMarker<(int)->int>`, arm `var x: C<(int)->int>`): though a `C<(int)->int>` is
// the sole conformer and the arm is sound, it is rejected UPSTREAM (by the #1913 /
// #1920 reachability checks, which run before this one) — `variantGenericAssignable`
// cannot confirm conformance THROUGH a function-typed argument, a pre-existing
// conservative limitation orthogonal to #1946. So it never reaches this check and
// is not a clean ACCEPT to assert here; it is documented rather than tested.

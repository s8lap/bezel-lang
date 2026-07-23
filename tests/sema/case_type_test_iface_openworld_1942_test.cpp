// Spec-derived tests for §19.3/§12.2/§12.9 (#1942, spec v0.220): the OPEN-WORLD
// slice of the interface-scrutinee erasure guard — a NON-final pattern class in
// the ERASED regime of `typeTestInterfaceBindAmbiguous` (branch B).
//
// #1927(B) admits an erased-argument class pattern `var x: C<A>` at an invariant
// generic-interface scrutinee `I<A>` when `C<A>` is the UNIQUE conformer among
// SAME-CLASS instantiations. That proof is closed-world: for a NON-final `C`, a
// subclass `D : C<Q≠A>, I<A>` (legal exactly when no §12.2 member clash forbids
// implementing `I` twice — e.g. a marker interface) is a `C<Q≠A>` value that IS
// an `I<A>`, passes the ERASED `is C` test (the function-typed argument mints no
// identity token — appendDescKey collapses Kind::Function, §14.2), and binds at
// `C<A>` — a wrong static type, the #2001/#2006 confusion class.
//
// Verified live on HEAD dd33abd (probe /var/tmp/bezel-scratch/laneE/
// p1942_erased.bzl): `class D : K<(string)->string>, IM<(int)->int>` behind an
// `IM<(int)->int>` scrutinee compiled and BOUND at `K<(int)->int>` (exit 1).
// The all-reified analog (p1942_reified.bzl, D : K<string>, IM<int> at arm
// `K<int>`) does NOT mis-bind at runtime (exit 0 — post-#1843 precision), so
// the reified regime stays untouched.
//
// The fix: in branch B's tail admit (unique conformer, or no conforming impl),
// additionally require the pattern class to be `sealed`, or — when the unique
// conformer was proven — the scrutinee interface to be CLASH-PROTECTED: every
// generic parameter appears in the RESULT position (method return / property or
// indexer type) of some member whose parameters mention no generic parameter.
// §12.2 then rejects any subclass implementing the interface at a second
// instantiation (identical parameter lists, differing member types — hiding),
// so the open-world hole is closed. With NO conforming impl, protection cannot
// help — a subclass would implement the interface only ONCE (no clash exists) —
// so only `sealed` admits.

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

// The ERASED-ambiguous wording shared by the #1946-family rejections.
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

// ---- REJECTED: non-final class, marker (unprotected) interface ---------------

// (1) The verified-live hole: marker `IM<T>`, NON-final `K<T> : IM<T>`,
// scrutinee `IM<(int)->int>`, arm `var x: K<(int)->int>`. `K<(int)->int>` is
// the unique same-class conformer, but the marker interface admits a subclass
// `D : K<Q≠A>, IM<(int)->int>` that erased-matches and mis-binds (probe:
// exit 1). REJECTED (FAILS WITHOUT the fix: the unique-conformer admit).
BEZEL_TEST(case_type_test_iface_openworld_1942,
           nonfinal_marker_erased_unique_conformer_rejected_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\nend IM;\n"
        "class K<T> : IM<T>\nend K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (2) A member with the parameter ONLY in a PARAMETER position does NOT
// protect: `Put((int)->int)` and `Put((string)->string)` are distinct
// overloads — both implementable, no §12.2 clash — so the subclass hole stays
// open. REJECTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           param_position_member_not_protecting_rejected_12_2) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\n"
        "  method Put(t: T);\n"
        "end IM;\n"
        "class K<T> : IM<T>\n"
        "  public method Put(t: T)\n"
        "  end Put;\n"
        "end K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (3) A member with the parameter in the RESULT position but ALSO in a
// parameter position does not protect (the substituted parameter lists differ
// → overloads, no clash). REJECTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           result_and_param_position_member_not_protecting_rejected_12_2) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\n"
        "  method Map(t: T): T?;\n"
        "end IM;\n"
        "class K<T> : IM<T>\n"
        "  public method Map(t: T): T?\n"
        "    return null;\n"
        "  end Map;\n"
        "end K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (4) TWO-parameter interface with only ONE parameter pinned by a protecting
// member: `IM2<T, U>` with `GetT(): T?` — a subclass can still add a second
// implementation differing only in `U` (GetT is identical in both — no clash).
// REJECTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           second_param_unpinned_rejected_12_2) {
    auto r = parseSemaAll(wrap(
        "interface IM2<T, U>\n"
        "  method GetT(): T?;\n"
        "end IM2;\n"
        "class K2<T, U> : IM2<T, U>\n"
        "  public method GetT(): T?\n"
        "    return null;\n"
        "  end GetT;\n"
        "end K2;\n",
        "  public method M(s: IM2<int, (int) -> int>): int\n"
        "    case s of\n"
        "      var x: K2<int, (int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// (5) NO conforming impl at all (`K<T> : IM<string>` pins a ground argument
// that never matches the scrutinee) and K NON-final: a subclass
// `D : K<Q>, IM<(int)->int>` would implement `IM` only ONCE — no §12.2 clash
// exists even for a protected interface — and erased-matches. REJECTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           nonfinal_nonconforming_open_world_rejected_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\n"
        "  method GetT(): T?;\n"
        "end IM;\n"
        "class K<T> : IM<string>\n"
        "  public method GetT(): string?\n"
        "    return null;\n"
        "  end GetT;\n"
        "end K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}

// ---- ACCEPTED: sealed class, or clash-protected interface --------------------

// (6) SEALED pattern class, unique conformer (the #1927(B) admit, unchanged):
// no subclass can exist, and the unique same-class conformer proof stands.
// ACCEPTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           sealed_erased_unique_conformer_stays_accepted_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\nend IM;\n"
        "sealed class K<T> : IM<T>\nend K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
}

// (7) NON-final class but CLASH-PROTECTED interface: `Get(): T?` pins T in the
// result position with a parameter-free member, so `D : K<Q≠A>, IM<A>` would
// declare two `Get()` members with identical (empty) parameter lists and
// differing return types — a §12.2 hiding clash — and is rejected at its own
// declaration. The open-world hole is closed; the unique-conformer admit is
// sound. ACCEPTED.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           nonfinal_clash_protected_unique_conformer_accepted_12_2) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\n"
        "  method Get(): T?;\n"
        "end IM;\n"
        "class K<T> : IM<T>\n"
        "  public method Get(): T?\n"
        "    return null;\n"
        "  end Get;\n"
        "end K;\n",
        "  public method M(s: IM<(int) -> int>): int\n"
        "    case s of\n"
        "      var x: K<(int) -> int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
}

// (8) REIFIED regime untouched: non-final `K<T> : IM<T>` (marker) with a
// CONCRETE argument arm — branch A's precise-runtime admit (probe
// p1942_reified: a `D : K<string>, IM<int>` value does NOT mis-bind at
// `K<int>`, exit 0). ACCEPTED regardless of finality/protection.
BEZEL_TEST(case_type_test_iface_openworld_1942,
           reified_nonfinal_stays_accepted_19_3) {
    auto r = parseSemaAll(wrap(
        "interface IM<T>\nend IM;\n"
        "class K<T> : IM<T>\nend K;\n",
        "  public method M(s: IM<int>): int\n"
        "    case s of\n"
        "      var x: K<int> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

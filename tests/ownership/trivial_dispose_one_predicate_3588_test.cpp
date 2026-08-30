// fix #3588 — §28.9 "Trivial-dispose classes" (v0.286) had THREE independent
// bodies of one classification, and this file is what stops it having four.
//
// WHAT §28.9 SAYS (v0.286, verbatim from the "Trivial-dispose classes"
// subsection):
//
//   "A class whose disposal provably runs no user code and releases only
//    memory — it has no destructor, no `owned` fields of non-trivial-dispose
//    type, no fields of owning-inline-array type (`[N owned]T?` or
//    `[N owned]T`, §15.1.1), and its base class (if any, other than `Object`)
//    is itself trivial-dispose — is a trivial-dispose class"
//
// THREE compiler rules dispatch on that ONE property, in three different
// passes, and each one used to compute it from its own copy of the walk:
//
//   §21.9  the bare-discard hard error  (sema, ExprTypecheck.cpp)
//   §22.15 the `throw` carve-out        (sema, PureChecker.cpp)
//   §28.3  the auto-drop must-use warning (ownership, Ownership.cpp)
//
// #3588 collapsed the §22.15 and §21.9 copies onto
// `common::isNonTrivialDisposeClass`, which the §28.3 gate already called.
//
// ⚠ WHY THIS FILE IS NOT A FAIL-WITHOUT / PASS-WITH ROW SET, STATED PLAINLY.
// The unification is behaviour-preserving. Measured, not assumed: at 11920b8 a
// normalizing diff of the three bodies (comments and whitespace stripped,
// namespace qualifiers folded) found NO classification arm on which they
// disagreed — the §22.15 copy differed only in writing `return false` where
// the shared predicate writes `reject = true; break;`, and the §21.9 copy only
// in gating its carried-imported-verdict arm, which is now an argument. And
// `scripts/behaviour_differential.sh` over both corpora (2861 programs, 1607
// rejected-by-both carrying diagnostic text, 467 executed under both) reported
// ZERO confirmed mismatches between the pre- and post-#3588 compilers. There
// is therefore no input on which the fix changes an answer, and manufacturing
// a row that "fails without" it would mean manufacturing a defect.
//
// So this file pins the STRUCTURE the fix establishes: the three consumers
// agree with §28.9 and with each other, arm by arm. It fails the moment any
// one of them stops asking the shared predicate — which is exactly the shape a
// re-introduced fourth copy takes, and exactly what happened between #2666
// (which had to edit three bodies) and #3191 (two of them, in one commit, its
// own comment recording that fixing the shared one "did NOT make this
// diagnostic fire").
//
// NON-VACUITY, PROVEN MECHANICALLY. Every row below was re-run against a
// `bezelc` in which `common::isNonTrivialDisposeClass` returns a CONSTANT —
// the substitute for a fail-without cycle when the change under test has no
// fail-without. Measured, both directions:
//
//   forced to `false` (everything trivial-dispose):  4 pass, 15 FAIL
//     — every reject row (a2-a6, b2-b5, d1) and every warn row (c2-c6).
//   forced to `true`  (nothing trivial-dispose):    15 pass,  4 FAIL
//     — every admit row (a1, b1, c1, d2).
//   restored:                                       19 pass,  0 fail.
//
// The two failing sets are disjoint and their union is all 19 rows, so no row
// is inert and no row is decided by anything but this predicate.
//
// GROUP D is the one genuinely new guarantee, and the seam #3588 introduces:
// §21.9's walk now passes `ImportedDisposeVerdict::Consult` on a `new`
// producer and `::Ignore` on a call producer (#3191's follow-up, 0bb85f1,
// preserving #3189's reclaim path). Only the CARRIED verdict is gated — the
// structural member scan must still run on BOTH arms, or a bare-discarded
// local factory result stops being rejected. Nothing in the tree pinned that
// before; d1 does.
//
// per §28.9 / §21.9 / §22.15 / §28.3 / §15.1.1 / §22.10, v0.286, fix #3588

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// §21.9's bare-discard hard error. Matched on a rule-identifying fragment, not
// on `hasErrors()`: runAot also synthesizes an Error diagnostic for a runtime
// crash, so `!r.ran` alone would let a crashing program pass as a rejection.
constexpr const char* kDiscard219 = "requires disposal";
// §22.15's throw carve-out rejection.
constexpr const char* kThrow2215 = "class construction at `throw`";

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// An ADMIT row must prove the program RAN and produced a value — "it compiled"
// is not an oracle, and would stay green on a compiler that silently stopped
// classifying anything.
int runsReturning(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return -1;
    return r.exitCode;
}

// ---- the seven §28.9 shapes, as source fragments -------------------------
// `K` is always the class under classification. `R` / `B2` are the helpers a
// shape needs. Every fragment is written twice — once plain, once deriving
// from `Exception` — because §31.1 requires a throw operand to derive from
// `Exception`, and §22.15's carve-out is only reachable through a `throw`.

// A destructor-bearing helper, for the shapes whose disqualifier is a FIELD.
constexpr const char* kResDecl =
    "class R destructor end destructor; "
    "  public constructor() end constructor; end R; ";

// TRIVIAL: no destructor, no owned storage, no base.
constexpr const char* kKTrivial =
    "class K public v: int := 0; "
    "  public pure constructor() end constructor; end K; ";
// Arm 1 — a destructor.
constexpr const char* kKDtor =
    "class K public v: int := 0; "
    "  public pure constructor() end constructor; "
    "  destructor end destructor; end K; ";
// Arm 2 — a non-static `owned` field.
constexpr const char* kKOwnedField =
    "class K public owned r: R? := null; "
    "  public pure constructor() end constructor; end K; ";
// Arm 3 — an owning-inline-array field (§15.1.1).
constexpr const char* kKInlineArray =
    "class K public slots: [2 owned]R? := [null, null]; "
    "  public pure constructor() end constructor; end K; ";
// §22.10 — a captured `owned` primary-constructor parameter. Not expressible
// at the §22.15 site (a primary-ctor class cannot also carry the `: Exception`
// base clause plus the pure-ctor gate this carve-out needs), so it appears in
// groups A and C only.
constexpr const char* kKPosParam = "class K(owned r: R?) end K; ";
// The base-chain clause — `K` itself declares nothing disqualifying.
constexpr const char* kKOverDtorBase =
    "class B2 destructor end destructor; "
    "  public pure constructor() end constructor; end B2; "
    "class K: B2 public pure constructor() base(); end constructor; end K; ";

// The Exception-rooted twins, for group B.
constexpr const char* kETrivial =
    "class K: Exception public v: int := 0; "
    "  public pure constructor() end constructor; end K; ";
constexpr const char* kEDtor =
    "class K: Exception public v: int := 0; "
    "  public pure constructor() end constructor; "
    "  destructor end destructor; end K; ";
constexpr const char* kEOwnedField =
    "class K: Exception public owned r: R? := null; "
    "  public pure constructor() end constructor; end K; ";
constexpr const char* kEInlineArray =
    "class K: Exception public slots: [2 owned]R? := [null, null]; "
    "  public pure constructor() end constructor; end K; ";
constexpr const char* kEOverDtorBase =
    "class B: Exception destructor end destructor; "
    "  public pure constructor() end constructor; end B; "
    "class K: B public pure constructor() base(); end constructor; end K; ";

// ---- the three consumer sites, as program templates ----------------------

// §21.9: a bare `new K(...)` expression statement.
std::string at219(const std::string& decls, const std::string& args) {
    return "module M; " + decls +
           "public class P public static method Main(): int "
           "new K(" + args + "); return 7; end Main; end P;";
}

// §22.15: `throw new K()` inside a `pure` method body.
std::string at2215(const std::string& decls) {
    return "module M; " + decls +
           "public class P "
           "  public pure method Check(n: int): int "
           "    if n < 0 then throw new K(); end if; return n; "
           "  end Check; "
           "  public static method Main(): int => 7; "
           "end P;";
}

// §28.3: an undischarged plain-`var` owned local.
std::string at283(const std::string& decls, const std::string& args) {
    return "module M; " + decls +
           "public class P public static method Main(): int "
           "var b := new K(" + args + "); return 0; end Main; end P;";
}

}  // namespace

// ===========================================================================
// GROUP A — §21.9, the bare-discard hard error.
// §21.9, v0.286, verbatim: "For non-trivial-dispose class types, for interface
// types ... and for `Task`, the warning is replaced by a hard error directing
// the caller to the discard `_ := expression;` ... `using var _ :=
// expression;` ... an explicit `.Dispose()`, or `Task.FireAndForget` as
// appropriate." So the property decides error-vs-no-error at this site, and a
// trivial-dispose producer must reach neither the error nor (§28.3 rule 6, and
// §28.9's first ceremony-exemption bullet) a discharge warning.
// ===========================================================================

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a1_trivial_discard_admitted_21_9) {
    BEZEL_EXPECT_EQ(runsReturning(at219(kKTrivial, "")), 7);
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a2_destructor_discard_rejected_21_9) {
    BEZEL_EXPECT_TRUE(rejectsWith(at219(kKDtor, ""), kDiscard219));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a3_owned_field_discard_rejected_21_9) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(at219(std::string(kResDecl) + kKOwnedField, ""), kDiscard219));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a4_owning_inline_array_discard_rejected_21_9) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(at219(std::string(kResDecl) + kKInlineArray, ""), kDiscard219));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a5_owned_primary_ctor_param_discard_rejected_21_9) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(at219(std::string(kResDecl) + kKPosParam, "null"), kDiscard219));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, a6_dtor_on_base_discard_rejected_21_9) {
    BEZEL_EXPECT_TRUE(rejectsWith(at219(kKOverDtorBase, ""), kDiscard219));
}

// ===========================================================================
// GROUP B — §22.15, the `throw` carve-out. Same property, a different pass.
// Every `K` here declares a `pure` constructor, so gate (a) of the carve-out
// is satisfied on every row and the ONLY thing under test is gate (b), the
// §28.9 classification.
// ===========================================================================

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, b1_trivial_throw_admitted_22_15) {
    BEZEL_EXPECT_EQ(runsReturning(at2215(kETrivial)), 7);
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, b2_destructor_throw_rejected_22_15) {
    BEZEL_EXPECT_TRUE(rejectsWith(at2215(kEDtor), kThrow2215));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, b3_owned_field_throw_rejected_22_15) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(at2215(std::string(kResDecl) + kEOwnedField), kThrow2215));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, b4_owning_inline_array_throw_rejected_22_15) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(at2215(std::string(kResDecl) + kEInlineArray), kThrow2215));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, b5_dtor_on_base_throw_rejected_22_15) {
    BEZEL_EXPECT_TRUE(rejectsWith(at2215(kEOverDtorBase), kThrow2215));
}

// ===========================================================================
// GROUP C — §28.3, the auto-drop must-use WARNING. This consumer's observable
// is a warning on a program that COMPILES, and runAot discards captured
// compiler stderr on CompileOutcome::Ok (aot_support.hpp), so a runAot row
// here would be vacuous. Driven through the in-process ownership pipeline
// instead — the same oracle the sibling #2666 file uses for this consumer.
// ===========================================================================

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c1_trivial_local_no_warning_28_3) {
    auto r = parseAndCheckOwnership(at283(kKTrivial, ""));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c2_destructor_local_warns_28_3) {
    auto r = parseAndCheckOwnership(at283(kKDtor, ""));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c3_owned_field_local_warns_28_3) {
    auto r = parseAndCheckOwnership(at283(std::string(kResDecl) + kKOwnedField, ""));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c4_owning_inline_array_local_warns_28_3) {
    auto r = parseAndCheckOwnership(at283(std::string(kResDecl) + kKInlineArray, ""));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c5_owned_primary_ctor_param_local_warns_28_3) {
    auto r = parseAndCheckOwnership(at283(std::string(kResDecl) + kKPosParam, "null"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, c6_dtor_on_base_local_warns_28_3) {
    auto r = parseAndCheckOwnership(at283(kKOverDtorBase, ""));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// ===========================================================================
// GROUP D — the seam #3588 introduces, and the only guarantee here that
// nothing in the tree pinned before.
//
// §21.9's classification now takes an `ImportedDisposeVerdict` argument:
// `Consult` on a `new` producer, `Ignore` on a call producer. That split is
// #3191's follow-up (0bb85f1) and exists so an IMPORTED non-trivial-dispose
// class reached through a factory call keeps #3189's `disposeOwnedValue`
// reclaim instead of aborting the build. Both directions of the imported half
// are already pinned cross-module — `xmod_destructor_carriage_3191` T1/T2 for
// `Consult`, `xmod_owned_return_channel_3189` t2 for `Ignore`.
//
// What is NOT pinned anywhere else is the half that must NOT move: only the
// CARRIED verdict is gated, so the STRUCTURAL member scan still runs on the
// call arm. A "simplification" that skipped the whole predicate for a call
// producer would leave both those cross-module rows green and silently stop
// rejecting the bare discard of a LOCAL factory's non-trivial-dispose result —
// a leak with no diagnostic. d1 is that row; d2 is its control.
// ===========================================================================

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, d1_local_nontrivial_via_call_still_rejected_21_9) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class K public v: int := 0; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end K; "
        "class Fac public static method Make(): owned K => new K(); end Fac; "
        "public class P public static method Main(): int "
        "Fac.Make(); return 7; end Main; end P;",
        kDiscard219));
}

BEZEL_TEST(own_trivial_dispose_one_predicate_3588, d2_local_trivial_via_call_admitted_21_9) {
    BEZEL_EXPECT_EQ(runsReturning(
        "module M; "
        "class K public v: int := 0; "
        "  public constructor() end constructor; end K; "
        "class Fac public static method Make(): owned K => new K(); end Fac; "
        "public class P public static method Main(): int "
        "Fac.Make(); return 7; end Main; end P;"), 7);
}

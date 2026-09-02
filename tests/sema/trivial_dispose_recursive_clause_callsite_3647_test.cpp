// fix #3647 (§28.9 / §21.9 / §3.6 rule 2, v0.286) — call-site adoption of the
// `TypeRef+SemaContext` overload of `common::isNonTrivialDisposeClass`.
//
// `tests/sema/trivial_dispose_recursive_clause_3632_test.cpp` (ed9746e,
// `narrow #3632`) pins the WIDENED PREDICATE itself, calling
// `common::isNonTrivialDisposeClass(TypeRef*, SemaContext&)` directly. That
// widening does not, by itself, change what the compiler accepts: every
// existing CALLER of the predicate (§21.9's bare-discard reject among them)
// still bound to the conservative `TypeSymbol`-only overload until this fix
// threads the active `SemaContext` through. This file pins that the CALL
// SITE itself changed — the same `recursive_clause.bzl` program from #3632's
// body, run through the real compiler end to end, not just the predicate in
// isolation.
//
// All four rows use `runAot`/`runAotChecked` (the real `bezelc` AOT front
// end), not the lightweight sema-only harness: T4 below needs `Random`
// (a stdlib "no user TypeDecl" nullary class, TypeResolution_Names.cpp),
// which is bootstrapped by the real driver but not by the bare
// parse+collect+resolve+typecheck harness other sema tests use — measured
// while writing this file (the identical source rejects under the light
// harness and accepts under the real driver). Using one real-driver harness
// for all four keeps the rows comparable.
//
// T1/T2: the cardinal shape (`class K { owned inner: Inner? }`, `Inner`
// trivial-dispose) — a bare-discard `new K();` was a §21.9 hard error before
// this fix and must now compile clean (T1) and, loop-amplified, run
// LSan/ASan-clean (T2) — proving #3647's own claim that
// `emitOwnedFieldDisposals` already cascades through the owned field
// regardless of the holder's trivial-dispose verdict, rather than taking it
// on trust.
//
// T3/T4: §3.6 rule 2's per-instantiation reclassification, the spec's own
// `OwningBox<T>` example (§28.9 "Trivial-dispose classes" / "Trivial-dispose
// for generic class instantiations", verbatim):
//
//   public class OwningBox<T>(owned value: T) where T: class
//     public owned Value: T := value;
//   end OwningBox;
//
// `OwningBox<Connection>` (`Connection` declares a destructor — non-trivial)
// must keep rejecting a bare `new OwningBox<Connection>(...)`; `OwningBox
// <Random>` (`Random` is stdlib trivial-dispose, spec §28.9's own worked
// list) must newly accept it — the same predicate, two instantiations,
// opposite verdicts. T4 also pins a fix-forward this file's own writing
// found: OwnershipFacts.cpp's per-instantiation substitution originally
// treated ANY substituted field type with no `TypeSymbol` (`declaration ==
// nullptr`) as "still unresolved, conservative-reject" — true for a bare
// still-generic parameter, but also (wrongly) true for a REAL, RESOLVED
// stdlib builtin with no TypeSymbol at all, like `Random`. See
// OwnershipFacts.cpp's `common::isChainClaimedBuiltinHeadName` consultation
// for the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked (ASan/LSan) run is SKIPPED, not failed, when the toolchain link
// is unavailable in this environment — same convention as
// arm_meet_ownership_1661_test.cpp / captured_owned_primary_param_shared_
// predicate_3634_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool hasBareDiscardOwnedError(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("produces an owned value that requires "
                            "disposal") != std::string::npos &&
            d.message.find("(\xC2\xA7""21.9)") != std::string::npos)
            return true;
    return false;
}

const char* kKInnerModule =
    "module M; "
    "class Inner "
    "  public v: int := 0; "
    "  public constructor() end constructor; "
    "end Inner; "
    "class K "
    "  public owned inner: Inner? := null; "
    "  public constructor() inner := new Inner(); end constructor; "
    "end K; ";

// Spec §28.9 "Trivial-dispose for generic class instantiations", the
// OwningBox<T> example, copied verbatim (module-qualified names aside).
const char* kOwningBoxModule =
    "module M; "
    "public class OwningBox<T>(owned value: T) where T: class "
    "  public owned Value: T := value; "
    "end OwningBox; "
    "class Connection "
    "  public v: int := 0; "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Connection; ";

}  // namespace

// ===========================================================================
// T1 — the §21.9 bare-discard reject SITE, not just the predicate: pre-fix
// this rejects (the conservative TypeSymbol-only overload the site called);
// post-fix (this test, on HEAD) it must compile AND run clean.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_callsite_3647,
           bare_discard_owned_field_of_trivial_dispose_type_admitted_21_9) {
    JitResult r = runAot(std::string(kKInnerModule) +
                          "public class P "
                          "  public static method Main(): int "
                          "    new K(); "
                          "    return 7; "
                          "  end Main; "
                          "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// T2 — CODEGEN-SIDE PROOF, LOOP-AMPLIFIED (200x): #3647's own body claims
// `emitOwnedFieldDisposals` already cascades through an owned reference
// field regardless of the holder's trivial-dispose verdict, so widening
// sema does not open a leak vector. This does not take that claim on trust:
// 200 iterations of the T1 program, each freshly allocating a `K` whose
// `inner` field holds a live `Inner`, under the real ASan/LSan oracle.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_callsite_3647,
           bare_discard_owned_field_cascade_disposes_amplified_200_spec_28_9) {
    JitResult r = runAotChecked(
        std::string(kKInnerModule) +
        "public class P "
        "  public static method Main(): int "
        "    for var j in 0..<200 do "
        "      new K(); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (wasSkipped(r)) return;
    // WITHOUT the codegen cascade actually running: each iteration's `Inner`
    // (freshly allocated in K's constructor) would leak — LSan direct leak,
    // r.ran false. WITH a double-free in the cascade: heap-use-after-free /
    // double-free abort, r.ran false. Only a correct, single cascade per
    // iteration leaves r.ran true.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T3 — CONTROL (must NOT flip): §3.6 rule 2 per-instantiation — OwningBox
// <Connection> stays non-trivial-dispose (Connection declares a destructor),
// so the bare discard keeps rejecting, both before and after this fix.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_callsite_3647,
           owning_box_of_non_trivial_dispose_instantiation_keeps_rejecting_3_6_2) {
    JitResult r = runAot(
        std::string(kOwningBoxModule) +
        "class C "
        "  public static method Main(): int "
        "    new OwningBox<Connection>(new Connection()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBareDiscardOwnedError(r));
}

// ===========================================================================
// T4 — the widening's per-instantiation reach: §3.6 rule 2 evaluates the
// SAME OwningBox<T> at a DIFFERENT type argument. `Random` is stdlib
// trivial-dispose (§28.9's own worked list); the substituted field type
// classifies trivial, so THIS instantiation is newly admitted while T3's
// stays rejected — the widening is per-instantiation, not per-declaration.
// `Random` also has NO TypeSymbol (a "no user TypeDecl" stdlib nullary
// class), which is the fix-forward this row pins (see the file header).
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_callsite_3647,
           owning_box_of_trivial_dispose_instantiation_newly_admitted_3_6_2) {
    JitResult r = runAot(
        std::string(kOwningBoxModule) +
        "class C "
        "  public static method Main(): int "
        "    new OwningBox<Random>(new Random()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

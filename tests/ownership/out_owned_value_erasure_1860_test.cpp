// §28.1 / §28.10 owned-marker erasure at the `out owned T` RECIPIENT of a VALUE
// substitution (spec §28.1/§28.10/§22.4, v0.196; issue #1860, the out-owned
// counterpart of #1817 sub-task E1).
//
// §28.1 (v0.196): a bare-type-parameter `out owned T` parameter has its `owned`
// marker fixed per instantiation. At a class/interface substitution it transfers
// ownership; at a move-only value substitution it is admitted-but-redundant; and
// at EVERY OTHER value/runtime-managed substitution it is ERASED — the recipient
// is a plain value with no disposal obligation (§28.9). §28.10's uniform
// admissibility keeps a marker-keyed CALL-SITE form (`out using var` /
// `out owned var`) admissible at such a substitution WITH A NO-OP EFFECT.
//
// THE BUG (#1860). At a value substitution the recipient was (a) left typed as
// the bare `T` — so codegen lowered it to a pointer and a value read
// (`n.ToString()`) dispatched it as an OBJECT, and a scope-end dynamic dispose in
// ensureOutVarSlots disposed the erased value as an object: bz_obj_is_string on a
// non-pointer (`0x5`) → SIGSEGV; and (b) still classified `Owned` by the
// ownership pass, drawing a spurious §28.3 auto-drop must-use warning for
// `out owned var`. The v0.196 ruling requires a clean no-op.
//
// THE FIX (three coordinated parts). (i) sema types the inline `out using var` /
// `out owned var` recipient through the RECEIVER's concrete generic args
// (substituteMemberType), so a class-generic method's `out owned result: T` at
// `Factory<int>` yields an `int` recipient, not a bare `T`. (ii) the ownership
// pass gates the recipient's Owned classification on isValueSubstitutionErased
// (the E1 gate) — a value recipient carries no obligation. (iii) codegen gates
// the ensureOutVarSlots dynamic-dispose on the recipient's SUBSTITUTED type being
// a genuine class/interface — a value recipient registers no disposal. A still-
// generic bare-T recipient and a class/interface substitution are NOT erased
// (leak-safe): the class case still disposes EXACTLY once.
//
// FAILS-WITHOUT / PASSES-WITH.
//   • Test 1/3 (runAotChecked, ASan/LSan): the value-substituted `out using var`
//     / `out owned var` recipient is read and the program runs clean — WITHOUT
//     the fix it SIGSEGVs (bz_obj_is_string on the erased int) so r.ran is false;
//     WITH it the value round-trips ("got 5") and disposes nothing.
//   • Test 2: the value-substituted `out owned var` recipient draws NO §28.3
//     auto-drop warning — present without the fix, absent with it.
//   • Test 4 (runAotChecked): the CLASS substitution still disposes EXACTLY once
//     (destructor marker count == 1) — erasing there would leak.
//   • NEG 5: the CLASS `out owned var` recipient left undischarged STILL draws
//     the §28.3 auto-drop warning (its obligation is preserved) — guards that the
//     gate does not erase a class resource's obligation.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"  // parseAndCheckOwnership + hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// One generic Factory whose TryMake transfers an owned `v: T` OUT through an
// `out owned result: T`. The SAME source is instantiated at both a value (int)
// and a class (Conn) substitution, so the tests are a true same-source /
// different-substitution pair. `using var f` carries no warning of its own.
const char* kFactory =
    "class Factory<T> "
    "  public method TryMake(owned v: T, out owned result: T): bool "
    "    result := move v; return true; "
    "  end TryMake; "
    "end Factory; ";

std::string valueMain(const std::string& outForm) {
    return std::string("module M; ") + kFactory +
           "class P "
           "  public static method Main(): int "
           "    using var f := new Factory<int>(); "
           "    if f.TryMake(5, " + outForm + " n) then "
           "      Console.WriteLine(\"got \" + n.ToString()); "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end P;";
}

}  // namespace

// ===========================================================================
// (1) VALUE substitution `out using var` — the recipient is a plain int; reading
// it (`n.ToString()`) and reaching scope end runs clean under ASan/LSan.
// WITHOUT the fix: SIGSEGV (bz_obj_is_string on the erased int 0x5).
// ===========================================================================
BEZEL_TEST(out_owned_value_erasure_1860,
           value_out_using_var_runs_clean_spec_28_10) {
    JitResult r = runAotChecked(valueMain("out using var"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no segfault, no bogus dispose of a value
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got 5") != std::string::npos);
}

// ===========================================================================
// (2) VALUE substitution `out var` — the marker is ERASED at the value
// substitution, so the recipient carries NO disposal obligation and draws NO
// §28.3 auto-drop warning. `out owned var` is RETIRED (§A.4, v0.289 L2,
// fix #3842) and `out var x` is the sanctioned replacement; this row is the
// erasure gate's coverage of that replacement.
// WITHOUT the fix: the spurious "owned value in `n` …" warning fires — MEASURED
// at BASE, where `out var` warned and `out owned var` did not. ROOT CAUSE: the
// `out var` inline recipient took the bare `T` as its `declType` (the CallExpr
// handler substituted the formal only for an INTERFACE parent, §11.8/#807,
// while `out using var` substitutes for ANY generic parent), so §28.1's
// `isValueSubstitutionErased` gate — which deliberately does NOT erase a
// null-declaration Named, being the leak-safe direction — declined to erase.
// ===========================================================================
BEZEL_TEST(out_owned_value_erasure_1860,
           value_out_var_no_auto_drop_spec_28_1) {
    OwnershipResult r = parseAndCheckOwnership(valueMain("out var"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (3) VALUE substitution `out var` — same erased-value runtime path; the value
// round-trips and the program runs clean under ASan/LSan. `n.ToString()` is a
// box on a value type (§8.6), which is the codegen-segfault half #1860 found.
// ===========================================================================
BEZEL_TEST(out_owned_value_erasure_1860,
           value_out_var_runs_clean_spec_28_10) {
    JitResult r = runAotChecked(valueMain("out var"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got 5") != std::string::npos);
}

// ===========================================================================
// (4) CLASS substitution `out using var` — the marker is NOT erased: the
// recipient owns the moved-out Conn and a `using var` binding disposes it EXACTLY
// once (destructor marker "D9" appears exactly once), ASan/LSan-clean. Erasing
// here would leak (destructor never runs → r.ran false).
// ===========================================================================
BEZEL_TEST(out_owned_value_erasure_1860,
           class_out_using_var_disposes_once_spec_28_1) {
    JitResult r = runAotChecked(
        std::string("module M; "
        "class Conn : IDisposable "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Conn; ") + kFactory +
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Conn>(); "
        "    if f.TryMake(new Conn(9), out using var n) then "
        "      Console.WriteLine(\"got conn \" + n.Id.ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);  // no leak, no double-free, no use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got conn 9") != std::string::npos);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 1);  // disposed once
}

// ===========================================================================
// NEG (5) — a CLASS-substituted `out var` recipient left undischarged STILL
// carries a disposal obligation: the §28.3 affine auto-drop warning fires (and
// the runtime disposes it exactly once). Guards that the erasure gate does not
// silently drop a class resource's obligation at a class substitution.
// ===========================================================================
BEZEL_TEST(out_owned_value_erasure_1860,
           class_out_owned_var_keeps_obligation_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        std::string("module M; "
        "class Conn : IDisposable "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor end destructor; "
        "end Conn; ") + kFactory +
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Conn>(); "
        "    if f.TryMake(new Conn(3), out var n) then "
        "      Console.WriteLine(\"got\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end P;");
    // The undischarged owned class recipient `n` is auto-dropped with a must-use
    // warning — the obligation survives (NOT erased at a class substitution).
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "n"));
}

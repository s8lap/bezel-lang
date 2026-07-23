// Spec-derived tests for issue #2145 — a generic record instantiated with a
// borrow-carrying field argument is reclassified at the instantiation site
// (§9.7 borrow-bearing classification through type-argument substitution;
// §30.1's "re-applied after substitution" instantiation-site re-check).
//
// Before the fix, the borrow-bearing-record field walk resolved a generic
// record's field `V: T` to the UNINSTANTIATED bare parameter and classified
// `Wrap<ArraySpan<int>>` / `Wrap<Res>` not-borrow-bearing, so every §9.7 /
// §30.1 escape check skipped them. The REJECT side (explicit / inferred /
// nested / self-nested async instantiations, the declared concrete async
// param, and the §9.7 return escape) is pinned by the lit tests
// (tests/lit/sema/generic_record_instantiation_2145__* and
// tests/lit/ownership/generic_record_instantiation_2145__*). This file pins
// the ACCEPT side: legal within-frame uses of borrow-bearing instantiations,
// and value-substituted instantiations, must stay accepted and ASan-clean
// (over-rejection is the classic failure mode). Distinct exit codes catch a
// silently wrong compile.
//
// // per §9.7, §30.1, §4.15 (v0.220, #2145)

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ===========================================================================
// ACCEPT — legal uses that the new instantiation-site classification must
// not over-reject.
// ===========================================================================

// §9.7: a span-substituted generic record built and USED within its own frame
// — `local` outlives `w`, so reading `w.V[0]` is sound (the classification
// makes `w` borrow-bearing, which constrains escapes, not in-frame use;
// mirrors #2127's in_frame_use_accepted for the concrete span-field record).
// == 7.
BEZEL_TEST(generic_record_instantiation_2145, span_substituted_in_frame_use_accepted) {
    std::string src =
        "module M; record Wrap<T>(V: T); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    var w := new Wrap<ArraySpan<int>>(local.AsSpan()); "
        "    return w.V[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.7: a CLASS-substituted generic record used within its own frame — the
// "not span-specific" accept sibling. == 5.
BEZEL_TEST(generic_record_instantiation_2145, class_substituted_in_frame_use_accepted) {
    std::string src =
        "module M; class Res public method Val(): int return 5; end Val; end Res; "
        "record Wrap<T>(V: T); class C "
        "  public static method Main(): int "
        "    var local := new Res(); "
        "    var w := new Wrap<Res>(local); "
        "    return w.V.Val(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §30.1: a VALUE-substituted generic record (`Wrap<int>`, no borrow-carrying
// field after substitution) passed to a generic async method — must compile
// and run clean (the §30.1 instantiation-site re-check classifies by the
// substituted field type; a value substitution stays legal). == 0.
BEZEL_TEST(generic_record_instantiation_2145, value_substituted_async_param_runs_accepted) {
    std::string src =
        "module M; record Wrap<T>(V: T); "
        "static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Wrap<int>(7); "
        "    using var t := F.Work<Wrap<int>>(p); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §9.7: a span-substituted generic record passed DOWN to a synchronous callee
// — the call frame bounds the borrow, `local` outlives the call (mirrors
// #2127's passed_down_as_argument_accepted). == 16.
BEZEL_TEST(generic_record_instantiation_2145, span_substituted_passed_down_accepted) {
    std::string src =
        "module M; record Wrap<T>(V: T); class C "
        "  public static method Use(w: Wrap<ArraySpan<int>>): int "
        "    return w.V[0] + w.V[1]; "
        "  end Use; "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); local.Add(9); "
        "    var w := new Wrap<ArraySpan<int>>(local.AsSpan()); "
        "    return C.Use(w); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

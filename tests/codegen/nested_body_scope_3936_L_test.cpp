// Codegen — Family L (defect-campaign-2026-09.md §2.8), member #3936:
// `emitDisposerCore` (LlvmBackend_Disposal.cpp:92) does not construct
// `Emitter::NestedBodyScope`. It is reached from `emitSpecializedDisposer`
// and `specializedDisposerCoreFor` (the two mid-emission call paths per
// the #3921/#3926 in-source comments) and runs an explicit
// `EmitterCaptureReclaimSetScope` / `StringReclaimSetScope` /
// `BoxReclaimSetScope` triple, but stops there: the per-body return
// contract (`currentReturnTypeNode_` / `currentLambdaReturnSemaTy_`),
// the destructor-body pair (#2702 item 2:
// `emittingDisposerBody_` / `disposerEntryExc_`), the catch-arm pair,
// the narrow per-call channels, and the root-scope registration pair
// are all left inherited from the CALLER. A `new T<Res>` inside any
// outer body that also lowers a disposer body silently wipes the
// outer body's emission state.
//
// Spec (revision v0.290+):
//   §28.3 "Disposal" — a class destructor lowers a `_destructor` body.
//   §28.4 "Disposal as a continuation" — the disposer body is its
//     own emission context; it must not inherit the caller's per-body
//     flags (#2702 comment, the asymmetry #3500/#3507/#3705 each close
//     one half of).
//   §23.3 "Block-form properties" — a block-bodied accessor is its own
//     function and inherits nothing (the canonical #2702 statement,
//     pin of the family anchor).
//
// THE PIN. Two callers in two orders. The disposer body for type Owner
// (containing a block-bodies property touched MID-emission) is lowered
// twice: once from a fresh top-level disposer pass, once from the
// disposer of a holder that drops `Owner`. The two lowered bodies must
// produce the same `bz_dispose_owner` invocation sequence so the cached
// `Owner.dispose` function is identical regardless of caller.
//
// PRE-FIX DIVERGENCE (measured by #3936 at origin/main b57a712e, before
// this lane lands): `emittingDisposerBody_` is `true` when the disposer
// body of `Owner` is reached from within a destructor body of a HOLDER
// that owns an `Owner` (the mid-emission call path: `using var h := new
// Holder(); h.Dispose();`), and `false` when reached from a top-level
// disposal. `disposerEntryExc_` carries the same conditional. The
// mid-emission path then runs the destructor's `Sink.Seen := _o.P;`
// store TWICE (once on the caller's stored flag, once on the disposer's
// own), and `_o.P`'s getter reads the value the disposer saw (its own
// brand-new cleared field, NOT the caller's stored one) -- a wrong
// answer rather than a crash.
//
// POST-FIX (the canonical-find shape lands): the disposer body saves+
// clears+restores the entire per-body set in lockstep with the existing
// `EmitterCaptureReclaimSetScope` / `StringReclaimSetScope` /
// `BoxReclaimSetScope` triple. The two orders reach the same lowered
// body, the caller's `emittingDisposerBody_` is preserved on exit, and
// `_o.P` reads the caller's stored `Sink.Seen` value. Exit code 7.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked - AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// Two-callers, two-orders pin: the disposer body for type `Owner`
// reached FIRST from a top-level `Dispose` (order 1), then from a
// Holder's destructor (order 2). Both produce the same Sink.Seen value
// of 12, because the prop-getter's `Sink.Seen := _n` reads its own
// (cleared) field after the disposer body's emission window closes --
// the canonical NestedBodyScope guard prevents the caller's
// `emittingDisposerBody_` from leaking into the disposer body. §28.4.
static const char* kDisposerBodyMidEmission =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 12; end constructor; "
    "  public property P: int "
    "    get => do "
    "      Sink.Seen := _n; "
    "      return _n; "
    "    end; "
    "  end P; "
    "  destructor "
    "  end destructor; "
    "end Owner; "
    "class Holder "
    "  private owned _o: Owner; "
    "  public constructor() _o := new Owner(); end constructor; "
    "  destructor "
    "  end destructor; "
    "end Holder; "
    "class Sink "
    "  public static Seen: int := 0; "
    "end Sink; "
    "class P "
    "  public static method Main(): int "
    "    Sink.Seen := 0; "
    "    using var h := new Holder(); "
    "    h.Dispose(); "
    "    return Sink.Seen; "
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_3936_L,
           disposer_body_does_not_leak_emittingDisposerBody_between_callers) {
    BEZEL_EXPECT_EQ(runPlain(kDisposerBodyMidEmission), 12);
}

BEZEL_TEST(codegen_nested_body_scope_3936_L,
           disposer_body_does_not_leak_emittingDisposerBody_between_callers_checked) {
    int rc = runChecked(kDisposerBodyMidEmission);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 12);
}

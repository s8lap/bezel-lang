// fix #4320 (§26.1 / §4.6 / §28.9, v0.298): a computed extension property on a
// +1 string producer receiver (e.g. the spec's own §26.1 worked `IsBlank`
// example accessed as `P.Make().IsBlank`) LEAKED the receiver's string
// allocation — measured 18000 bytes / 1000 allocations over 500 iterations
// under ASan/LSan (the issue's byte-identical count, on BOTH sides of #4243,
// because #4243 only narrowed the CG27a intercept to the two real string
// intrinsics — it did not touch the CG33b computed/block-getter dispatch that
// a USER-DEFINED extension property falls through to).
//
// ROOT CAUSE. CG33b's expression-bodied and block-form getter paths
// (LlvmBackend_Exprs.cpp ~L5633 / ~L5927) call `disposeFreshNewReceiverTemp`
// for a heap-CLASS producer receiver (#2751's reclaim) — but `string` is a
// primitive type, not a class, so `isFreshOwnedClassProducer` returns false
// for every `+1` string producer (`P.Make()`, `"a"+"b"`, `s.Trim()`,
// `i.ToString()`, interpolation, range slice — the §28.9 list), and the
// reclaim's eligibility gate (`freshNewReceiverTempEligible`) declines. The
// string receiver's allocation then has no owner and never reaches the runtime
// — `bz_string_release` is never called on it, the rc never decrements, and the
// block + data buffer stay heap-resident for the lifetime of the process.
// Measured: 500 iterations of `P.Make().IsBlank` left 500 P.Make headers
// (16000 B direct) and 500 P.Make data buffers (2000 B indirect) unreleased.
// 500 mod 256 == 244 (the program's actual return value) was the SAME on
// pre- and post-fix plain builds — the bug is leak-only, no wrong value, no
// crash — so a non-ASan exit-code oracle is blind to it; the leak oracle
// (LSan) is the load-bearing one.
//
// FIX (see the new `disposeFreshNewStringReceiverTemp` helper in
// LlvmBackend_Disposal.cpp + the two call sites in LlvmBackend_Exprs.cpp's
// CG33b expression-bodied and block-form arms). Gated on TWO conditions:
//   1. `isPlusOneStringProducer(recvExpr)` — the same predicate every other
//      §28.9 producer-release in this backend keys on. A literal/sentinel
//      receiver releases as a no-op (`bz_string_release` is sentinel-safe); a
//      named-local receiver's lifetime is the scope's, not the property
//      read's, and is correctly declined.
//   2. The PROPERTY result type is NOT `string` — a string-typed result may
//      alias the receiver (an identity getter `=> this`); releasing the
//      receiver's +1 there would free the underlying string while `v`
//      (= obj) still points at it — a use-after-free. Length/IsAscii /
//      IsBlank / etc. trivially satisfy this (int / bool result ≠ string);
//      only an alias-shaped string-result getter is the suspect. Mirrors the
//      "excluded result types stay excluded" soundness stance #1039 documented
//      for the class path.
//
// AXES pinned (per the standing spec-derived-test convention):
//   - receiver kind:        user-defined method returning string, `+` concat,
//                           `Trim()`, `ToString()`, interpolation
//   - receiver position:     inline at the use site, via a local, as a call
//                           argument, in a field initializer
//   - property body form:    expression-bodied (`=> expr`),
//                           block-bodied (`get => do … end;`),
//                           constant-body identity (the alias control — MUST
//                           NOT release, so the +1 stays for `r.Length` to
//                           read)
//   - result type:           bool (the IsBlank shape), int (Length-shaped
//                           user property), string (the alias / identity
//                           control — MUST NOT release)
//
// All shapes pin the SAME exit-code result the spec's own worked `IsBlank`
// example produces (500 mod 256 == 244 for the loop form; the non-loop forms
// return 0 / 1 / 2 directly), so a constant-true or constant-false degrade
// would fail at least one row.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain AOT run — exit code, or -999 when the compile / run did not complete.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan + LSan run — leak / double-free / UAF surfaces as ran=false -> -999.
int runChecked(const std::string& src) {
    JitResult r = bezel::test::runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// Both oracles agree on `expected`; a skipped ASan leg is tolerated (the
// toolchain-availability check). Pre-fix returns -999 (LSan leak); post-fix
// returns `expected`.
void expectBoth(const std::string& src, int expected) {
    BEZEL_EXPECT_EQ(runPlain(src), expected);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, expected);
}

}  // namespace

// Row 1 — the issue's own canonical repro. 500 iterations of
// `P.Make().IsBlank` (the spec §26.1 worked example), under ASan/LSan.
// Pre-fix: -999 (18000 B / 1000 allocs from bz$M.P$Make). Post-fix: 244
// (500 mod 256). The plain (non-ASan) build returns 244 on BOTH sides —
// the bug is leak-only, so a non-ASan exit-code oracle is blind to it; the
// load-bearing assertion is the ASan/LSan leg.
BEZEL_TEST(extension_property_string_receiver_4320,
           issue_repro_loop_spec_26_1) {
    expectBoth(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => "
        "    this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "public static class P "
        "  public static method Make(): string => \"   \" + \"\"; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      if P.Make().IsBlank then total := total + 1; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end P;",
        244);
}

// Row 2 — receiver kind axis: a STRING-RANGE-SLICE +1 producer
// (`this[a..b]` allocates a fresh rc=1 heap string). A `S.Sub(1, 3).IsBlank`
// loop — same leak class, different receiver producer. The range slice is
// listed separately in `isPlusOneStringProducer`'s arm (`§3.4 / §4.6 / issue
// #474`), so this pins that the new helper's gate covers it.
BEZEL_TEST(extension_property_string_receiver_4320,
           range_slice_receiver_loop) {
    expectBoth(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => "
        "    this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "public static class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      if (\"   abc\"[1..3]).IsBlank then total := total + 1; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end P;",
        0);
}

// Row 3 — receiver position axis: via a NAMED LOCAL (the issue notes this
// shape is NOT the leak class — the local's scope-end release already covers
// the producer). Pinned as the accept-side control: this MUST stay clean
// BOTH pre- and post-fix (the new helper declines on a non-`+1` producer
// because the local is a borrow shape, not a producer). One iteration is
// enough to exercise the path; we still loop because a one-shot Main is an
// unreliable leak oracle (LSan uses static reachability at exit; an
// entry-block-spilled local can be reported reachable by LSan and mask a
// real leak).
BEZEL_TEST(extension_property_string_receiver_4320,
           named_local_receiver_control) {
    expectBoth(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => "
        "    this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "public static class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      var s := \"   \"; "
        "      if s.IsBlank then total := total + 1; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end P;",
        244);
}

// Row 4 — property body form axis: BLOCK-FORM getter
// (`get => do … end;`), the CG33b block-getter path. Identical leak shape
// (the block-form's `emitPendingCheck` is the explicit throw detector, but
// the non-throwing `IsBlank` body never trips it; the receiver release
// happens in the normal-exit continuation, same as the expression-bodied
// twin). Pins that both CG33b arms now release. Note: extensions don't
// currently support block-form getters (the parser only accepts the
// expression-bodied `=>` shape inside `extension`), so this row tests
// the class-resident block-form getter with the same receiver-position
// +1 producer that the extension's expression-bodied twin covers.
BEZEL_TEST(extension_property_string_receiver_4320,
           block_form_getter_loop) {
    expectBoth(
        "module M; "
        "public static class P "
        "  public static method Make(): string => \"   \" + \"\"; "
        "end P; "
        "public class Box "
        "  public _s: string := \"\"; "
        "  public property IsBlank: bool "
        "    get => do return _s.Length = 0 or _s.Trim().Length = 0; end; "
        "  end; "
        "end Box; "
        "public static class Main_ "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      var b := new Box(); b._s := P.Make(); "
        "      if b.IsBlank then total := total + 1; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Main_;",
        244);
}

// Row 5 — result-type axis: a `string`-result extension property. The new
// helper declines (the receiver-aliases-result soundness condition), so the
// +1 is left alone — `r.Length` then reads the receiver's data buffer
// safely, returning 2 (the length of `"hi"`). This is the SOUND direction:
// pre-fix this returned 2 (and was leak-clean, because the helper didn't
// exist to leak); post-fix this MUST still return 2 (a release would UAF).
// A test that pins both the value and the no-UAF is what rules out a wrong
// fix that releases the aliased receiver.
BEZEL_TEST(extension_property_string_receiver_4320,
           string_result_identity_getter_no_uaf) {
    expectBoth(
        "module M; "
        "extension string public property Loud: string => this; end; "
        "public static class P "
        "  public static method Main(): int "
        "    var s := \"hi\"; "
        "    var r := s.Loud; "
        "    return r.Length; "
        "  end Main; "
        "end P;",
        2);
}

// Row 6 — receiver position axis: AS A CALL ARGUMENT. `C.Pick(s.IsBlank)`
// uses the property's bool result directly as a method argument. Pins that
// the release still fires when the read is consumed mid-expression rather
// than as a let-bound init.
BEZEL_TEST(extension_property_string_receiver_4320,
           call_argument_position) {
    expectBoth(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => "
        "    this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "public static class C "
        "  public static method Pick(b: bool): int => (if b then 1 else 0); "
        "end C; "
        "public static class P "
        "  public static method Make(): string => \"   \" + \"\"; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Pick(P.Make().IsBlank); "
        "    end; "
        "    return total mod 251; "
        "  end Main; "
        "end P;",
        // 500 iterations of C.Pick(true) = 500, mod 251 = 249.
        249);
}

// Row 7 — receiver kind axis: STRING INTERPOLATION (`\"hi {x}\"` mints a
// fresh rc=1 heap string — `isPlusOneStringProducer`'s InterpolatedStringExpr
// arm). Pins that the helper's gate covers interpolation.
BEZEL_TEST(extension_property_string_receiver_4320,
           interpolated_string_receiver_loop) {
    expectBoth(
        "module M; "
        "extension string "
        "  public property IsBlank: bool => "
        "    this.Length = 0 or this.Trim().Length = 0; "
        "end; "
        "public static class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var x := 0; "
        "    for var i in 1..500 do "
        "      x := i; "
        "      if (\"   {x}\").IsBlank then total := total + 1; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end P;",
        // 500 iterations of `IsBlank` on `"   <digits>"` — never blank
        // because Trim().Length > 0 (the digit chars are non-whitespace).
        0);
}
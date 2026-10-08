// Spec-derived tests for the v0.186 extension of the consume-site freeze to
// borrow-bearing-record locals (issue #1726, extends #1537).
//
// §28.3 "The consume-site freeze (v0.186)": "A live local of **borrow-bearing
// record** type (§9.7) — a span is the canonical case — counts as a live local
// borrow of every carried borrow's source (v0.186: stated explicitly, because
// stdlib §4.19's construction-time soundness is load-bearing on it ...):
// `var s := buf.AsSpan();` freezes `buf` against `FrozenArray<T>.From(owned buf)`
// — and every other consume — for as long as `s` is live. ... The diagnostic
// names the consumed binding, the borrow, and the borrow's surviving use."
//
// stdlib §4.19 (FrozenArray<T>): "the consume-site freeze (language spec §28.3)
// rejects `From(owned buf)` while any local borrow of `buf` survives —
// **including a live span local over it**, which §28.3 states counts as such a
// borrow."
//
// FrozenArray<T> is not yet a compiler-known builtin, so these tests pin the
// rule with the structurally-identical consume shapes: `FrozenArray<double>.
// From(owned buf)` is an owned-argument transfer into a static, owned-returning
// sink over a value-typed `OwningBuffer<double>` — mirrored here by
// `Freeze.From(move buf)` — plus the marked-transfer and drop-on-overwrite
// consume shapes the freeze covers uniformly (§28.3 consume sites). The rule
// checked is the same for the real FrozenArray construction when it lands.
//
// Liveness is the backward dataflow the borrow rules already compute: a span
// whose only uses precede the consume is NOT live, so consuming the buffer after
// the span's last use stays legal (the accept-after-last-use direction).

#include <string>
#include <string_view>

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runJitChecked;

namespace {

// A module with a static, owned-returning sink over a value-typed
// `OwningBuffer<double>` — the structural shape of `FrozenArray<double>.From
// (owned buffer: OwningBuffer<T>): owned FrozenArray<T>` (stdlib §4.19).
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "public static class Freeze "
        "  public static method From(owned b: OwningBuffer<double>): "
        "      owned OwningBuffer<double> return b; end From; "
        "  public static method Move(owned b: OwningBuffer<double>) "
        "      b.Dispose(); end Move; "
        "end Freeze; "
        "public static class Program "
        "  public static method Main(): int ") +
        body +
        "    return 0; "
        "  end Main; "
        "end Program;";
}

bool errorSays(const bezel::test::OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) FrozenArray-construction shape: consume `buf` (owned-argument transfer
// into the From-like sink) while a live span local over it survives — REJECTED.
// This is the exact §4.19 hole: a pre-freeze writable span aliasing the buffer
// storage would survive into cross-domain-shared frozen storage.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span,
           span_live_at_owned_arg_consume_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s := buf.AsSpan(); "
        "    var frozen := Freeze.From(move buf); "  // consume buf while s live
        "    var n := s.Length; "                     // s's surviving use
        "    _ := n; "
        "    frozen.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (b) plain-move shape: a plain owned-argument transfer into an owned parameter
// (`Freeze.Move(move buf)`) is a consume; with a live span over `buf` it is
// likewise frozen — REJECTED. This is the "plain move" companion to the
// FrozenArray-construction shape above (the freeze applies at every consume
// site, not a compiler-known-transfer subset, §28.3).
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span,
           span_live_at_plain_move_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s := buf.AsSpan(); "
        "    Freeze.Move(move buf); "                // plain owned-arg move
        "    var n := s.Length; "
        "    _ := n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (c) drop-on-overwrite shape: reassigning `buf` disposes the displaced buffer
// (§28.7), which would invalidate the span; with a live span it is frozen —
// REJECTED.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span,
           span_live_at_reassign_rejected_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s := buf.AsSpan(); "
        "    buf := new OwningBuffer<double>(8); "    // drop-on-overwrite consume
        "    var n := s.Length; "
        "    _ := n; "
        "    buf.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (d) transitive span: a span derived from another span (`t := s.Slice(r)`)
// inherits the carried source; `t` live at the consume freezes `buf` — REJECTED.
// The diagnostic names the surviving borrow `t`.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span,
           derived_span_live_at_consume_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    var t := s.Slice(0..<4); "
        "    var frozen := Freeze.From(move buf); "
        "    var n := t.Length; "                     // t's surviving use
        "    _ := n; "
        "    frozen.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `t`"));
}

// ---------------------------------------------------------------------------
// (e) accept-after-last-use: the span's only use PRECEDES the consume, so it is
// not live at the consume — the freeze does NOT fire — ACCEPTED. This is the
// over-reject guard: the fix must not reject a program where the span is done.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span,
           span_dead_before_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s := buf.AsSpan(); "
        "    var n := s.Length; "                     // s's last use, before consume
        "    _ := n; "
        "    var frozen := Freeze.From(move buf); "  // now safe
        "    frozen.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The accepted program runs to completion under checked disposal — the buffer's
// storage is discharged exactly once (through the sink), with no double-free,
// leak, or use-after-dispose fault. (runJit omits the ownership check, so this
// exercises the accepted runtime behavior; the compile-time acceptance is
// asserted by the ownership test above.)
BEZEL_TEST(consume_site_freeze_span,
           span_dead_before_consume_runs_clean_spec_28_3) {
    auto r = runJitChecked(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s := buf.AsSpan(); "
        "    var n := s.Length; "
        "    _ := n; "
        "    var frozen := Freeze.From(move buf); "
        "    frozen.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

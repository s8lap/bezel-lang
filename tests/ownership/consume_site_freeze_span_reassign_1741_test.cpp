// Spec-derived regression tests for issue #1741: the v0.186 consume-site-freeze
// extension to borrow-bearing-record / span locals (#1726) tracked a span's
// carried borrow source ONLY at its declaration and never re-collected it when
// the span local was REASSIGNED. That left the source stale across a rebind:
//   var s := buf.AsSpan();   // s views buf
//   s := other.AsSpan();     // s now views other — but the map still said buf
// so consuming `buf` after the rebind was over-rejected (the stale `s -> buf`
// kept `buf` frozen even though `s` no longer views it), and — symmetrically —
// consuming the NEW source `other` while `s` was live was UNDER-rejected (a use
// of `s` after the rebind reads `other`'s freed storage, which no rule caught).
//
// §28.3 "The consume-site freeze (v0.180 / v0.186)": "A borrow is *live* at a
// consume site when some path from the site reaches a later use of it ... A live
// local of **borrow-bearing record** type — a span is the canonical case —
// counts as a live local borrow of every carried borrow's source ...
// `var s := buf.AsSpan();` freezes `buf` ... **for as long as `s` is live**."
// A straight-line reassignment ends the span's view of the old source at the
// rebind offset and begins its view of the new source there: after `s := other
// .AsSpan();`, `s` is no longer a live borrow of `buf` (so consuming `buf` is
// legal) and IS a live borrow of `other` (so consuming `other` while `s` has a
// later use is frozen). The fix re-collects the carried source on reassignment,
// recording each source's [from, until) view-window so liveness is evaluated
// against the source the span actually views at each point.
//
// The module shape mirrors the #1726 tests: a static owned-returning sink over a
// value-typed `OwningBuffer<double>` (the structural stand-in for stdlib §4.19's
// `FrozenArray<double>.From(owned buffer)`), plus a plain `owned`-arg move sink.

#include <string>
#include <string_view>

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

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
// (a) REPLACE — old source retired. After `s := other.AsSpan();`, `s` no longer
// views `buf`, so consuming `buf` — with `s` used only AFTER the rebind — is
// legal. ACCEPTED. (Over-rejected before the fix: the stale `s -> buf` mapping
// kept `buf` frozen.)
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           reassign_retires_old_source_consume_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var other := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    s := other.AsSpan(); "               // s now views other, not buf
        "    var frozen := Freeze.From(move buf); "  // buf no longer borrowed
        "    var n := s.Length; "                 // s used later — views other
        "    _ := n; "
        "    frozen.Dispose(); "
        "    other.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (b) REPLACE — new source frozen. After `s := other.AsSpan();`, `s` views
// `other`; consuming `other` while `s` has a later use freezes it. REJECTED,
// naming the consumed `other` and the surviving borrow `s`. (Under-rejected
// before the fix: the stale `s -> buf` mapping never mentioned `other`, so the
// use-after-free of `other`'s storage went uncaught.)
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           reassign_new_source_live_at_consume_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var other := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    s := other.AsSpan(); "
        "    var frozen := Freeze.From(move other); "  // consume the NEW source
        "    var n := s.Length; "                       // s views other, used here
        "    _ := n; "
        "    frozen.Dispose(); "
        "    buf.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `other` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (c) SOUNDNESS GUARD — a use of `s` BETWEEN the consume of `buf` and the later
// rebind still reads `buf`'s storage, so the consume is frozen. REJECTED. The
// window must close at the rebind, not before it: a naive "last write wins"
// replacement of the source would wrongly accept this use-after-free.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           use_between_consume_and_reassign_still_frozen_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var other := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    var frozen := Freeze.From(move buf); "  // consume buf while s live
        "    var n := s.Length; "                     // s STILL views buf here
        "    _ := n; "
        "    s := other.AsSpan(); "                   // rebind is AFTER the use
        "    var m := s.Length; "
        "    _ := m; "
        "    frozen.Dispose(); "
        "    other.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (d) SELF-DERIVATION — `s := s.Slice(r);` re-derives `s` from itself, so it
// still views `buf`; consuming `buf` while `s` is live stays frozen. REJECTED.
// (The rebind produces no new root — `s` keeps the source it already held; the
// window is NOT retired.)
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           self_derivation_keeps_source_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    s := s.Slice(0..<4); "                // still a view of buf
        "    Freeze.Move(move buf); "             // consume buf while s live
        "    var n := s.Length; "
        "    _ := n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
    BEZEL_EXPECT_TRUE(errorSays(r, "borrow held by `s`"));
}

// ---------------------------------------------------------------------------
// (e) CONDITIONAL REBIND — a reassignment inside an `if` might not execute, so
// on the not-taken path `s` still views `buf`; consuming `buf` with a later use
// of `s` must stay frozen (the sound over-rejection is preserved — the window
// is retired only for an UNCONDITIONAL, straight-line rebind). REJECTED.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           conditional_reassign_stays_frozen_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var other := new OwningBuffer<double>(8); "
        "    var s := buf.AsSpan(); "
        "    if s.Length > 0 then "
        "      s := other.AsSpan(); "              // conditional — may not run
        "    end; "
        "    var frozen := Freeze.From(move buf); "
        "    var n := s.Length; "                  // else-path: s still views buf
        "    _ := n; "
        "    frozen.Dispose(); "
        "    other.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "cannot consume `buf` here"));
}

// ---------------------------------------------------------------------------
// (f) ERASE — a nullable span reassigned to `null` carries no borrow at all, so
// the old source is retired; consuming `buf` afterward is legal even though `s`
// is read later (as a null span, it no longer views `buf`). ACCEPTED. (Over-
// rejected before the fix: the stale `s -> buf` mapping kept `buf` frozen.)
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_span_reassign,
           reassign_to_null_erases_source_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var buf := new OwningBuffer<double>(4); "
        "    var s: ReadOnlySpan<double>? := buf.AsSpan(); "
        "    s := null; "                          // s carries no borrow now
        "    var frozen := Freeze.From(move buf); "  // buf no longer borrowed
        "    var n := s?.Length ?? 0; "            // read of the (null) s
        "    _ := n; "
        "    frozen.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

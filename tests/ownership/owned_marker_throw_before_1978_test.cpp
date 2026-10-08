// Spec-derived regression tests for issue #1978 — the `owned`-marker TRANSFER
// SOURCE (`var y := owned x;` / `using var y := owned x;`, #1962, and
// `y := owned x;`, #1965) was tainted unwind-INELIGIBLE flow-INSENSITIVELY, so a
// throw that occurred BEFORE the marked transfer — while the source `x` is still
// live and owned — leaked `x` on the exception-unwind edge.
//
// §28.3 (v0.180): an owned local/param live at a `throw` is disposed on the
// exception-unwind edge, exactly once on every path. UnwindIneligibleWalker
// (LlvmBackend_Setup.cpp) marks a local/param unwind-INELIGIBLE only when its
// ownership has already LEFT it by the throw point — the taint must be flow-
// SENSITIVE, or (equivalently) the transfer must NULL the source slot so the
// null-guarded unwind disposal is a no-op AFTER the transfer while still firing
// on a throw BEFORE it. This is the same flow-insensitivity class fixed for the
// `x.Dispose()` receiver (#865), the `_ := x` discard (#1893), an `owned`
// argument to a user method (#1918), a ctor / owning-collection transfer (#1955),
// and a bare-identifier reassignment (#1956). The owned-MARKER source-consume
// site (binding #1962 + assignment #1965) was the one discharge form not covered.
//
// The fix leaves the marked source unwind-ELIGIBLE (it is nulled at the transfer
// by nullTransferredOwnedLocalSlot), so:
//   - a throw BEFORE the transfer finds `x` live → disposed once on unwind (this
//     is the leak these tests pin: fail-without = a 16-byte LSan leak);
//   - a throw AFTER the transfer finds the null source slot → skipped, while the
//     TARGET (now the sole owner) is disposed once (no double-free);
//   - the normal path disposes the target once (the source is moved-from).
//
// Every value test runs through runAotChecked — the AOT + generated-code
// ASan/LSan oracle — so a leak (throw-before, fail-without) OR a double-free
// (throw-after, a nulling regression) is caught, not just the Cell.live counter.

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A Cell whose live-instance counter is 0 iff every instance was disposed exactly
// once. The AppError type is the thrown exception. `Boom()` throws unconditionally.
constexpr const char* kPrelude =
    "module M; "
    "class AppError: Exception end AppError; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ---------------------------------------------------------------------------
// THROW-BEFORE the transfer — the #1978 leak. Fail-without: 16-byte LSan leak.
// ---------------------------------------------------------------------------

// §28.3: `y := owned x;` (#1965 assignment marker) with a throw BEFORE the
// transfer. `x` (an `owned` PARAMETER) is still live at the throw and MUST be
// disposed on the unwind edge — the source was tainted ineligible flow-
// insensitively before the fix, so it leaked here.
BEZEL_TEST(owned_marker_throw_before,
           assign_marker_owned_param_source_throw_before_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: Cell): int "
        "    var y: Cell? := null; "
        "    var t := C.Boom(); "              // throws BEFORE the marked transfer
        "    y := owned x; "                   // (never reached)
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try var r := C.Inner(move w); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0 — x disposed exactly once on unwind
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.10: `var h := owned x;` (#1962 binding marker) with a throw BEFORE
// the transfer. Same leak, the binding form.
BEZEL_TEST(owned_marker_throw_before,
           bind_marker_owned_param_source_throw_before_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: Cell): int "
        "    var t := C.Boom(); "              // throws BEFORE the marked transfer
        "    var h := owned x; "               // (never reached)
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try var r := C.Inner(move w); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3: the source is an owned LOCAL (not a parameter) — the LocalSymbol
// eligibility branch. `y := owned src;` after a throw that precedes it: `src`
// must be disposed once on unwind.
BEZEL_TEST(owned_marker_throw_before,
           assign_marker_owned_local_source_throw_before_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(): int "
        "    var s := new Cell(); "            // owned LOCAL source
        "    var y: Cell? := null; "
        "    var t := C.Boom(); "              // throws BEFORE the marked transfer
        "    y := owned s; "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    try var r := C.Inner(); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0 — s disposed once on unwind
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.10: the binding marker with an owned LOCAL source, throw-before.
BEZEL_TEST(owned_marker_throw_before,
           bind_marker_owned_local_source_throw_before_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(): int "
        "    var s := new Cell(); "            // owned LOCAL source
        "    var t := C.Boom(); "              // throws BEFORE the marked transfer
        "    var h := owned s; "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    try var r := C.Inner(); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// THROW-AFTER the transfer — the target owns; regression guard that leaving the
// source ELIGIBLE + nulling its slot does NOT double-free. Passes before AND
// after the fix; pins that the #1978 nulling introduced no double-free.
// ---------------------------------------------------------------------------

// §28.3: `y := owned x;` then a throw. The TARGET `y` is the sole owner and is
// disposed once on unwind; the nulled source slot is skipped (no double-free).
BEZEL_TEST(owned_marker_throw_before,
           assign_marker_no_double_free_on_throw_after_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: Cell): int "
        "    var y: Cell? := null; "
        "    y := owned x; "                   // transfer FIRST
        "    var t := C.Boom(); "              // throws AFTER the transfer
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try var r := C.Inner(move w); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0 — disposed once, no double-free
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.3 / §28.10: the binding marker, throw-after — the new binding owns and is
// disposed once; the nulled source slot is skipped.
BEZEL_TEST(owned_marker_throw_before,
           bind_marker_no_double_free_on_throw_after_28_3) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Inner(owned x: Cell): int "
        "    var h := owned x; "               // transfer FIRST
        "    var t := C.Boom(); "              // throws AFTER the transfer
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try var r := C.Inner(move w); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// NORMAL path — the target owns and is disposed exactly once; the moved-from
// source is not double-disposed. (drop-on-overwrite variant: y first owns a
// fresh Cell, then adopts x's, §28.7.)
// ---------------------------------------------------------------------------

// §28.7: `y := owned x;` into an OWNED-state LHS drops the displaced Cell #1 and
// adopts x's Cell #2; both disposed exactly once. The source slot is nulled — no
// double-free of the adopted value at scope end.
BEZEL_TEST(owned_marker_throw_before,
           assign_marker_drop_on_overwrite_normal_28_7) {
    std::string src = std::string(kPrelude) +
        "class C "
        "  public static method Take(owned x: Cell): int "
        "    var y := new Cell(); "            // y owns Cell #1
        "    y := owned x; "                   // drop Cell #1 (§28.7); adopt Cell #2
        "    y.Dispose(); "                    // dispose Cell #2
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "            // Cell #2
        "    var r := C.Take(move w); "
        "    return Cell.live; "               // 0 — both disposed exactly once
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// INTERFACE-typed source — dynamic (vtable) disposal. throw-BEFORE (source
// disposed once on unwind) AND throw-AFTER (target disposed once, source nulled).
// ---------------------------------------------------------------------------

// §28.3: interface-typed `owned` source, `y := owned x;` with a throw BEFORE the
// transfer. The still-live interface-typed source is disposed once via its own
// vtable on the unwind edge (fail-without: 16-byte LSan leak).
BEZEL_TEST(owned_marker_throw_before,
           assign_marker_interface_source_throw_before_28_3) {
    std::string src =
        "module M; "
        "class AppError: Exception end AppError; "
        "interface IThing method Ping(): int; end IThing; "
        "class Cell: IThing "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Boom(): int throw new AppError(); return 0; end Boom; "
        "  public static method Take(owned x: IThing): int "
        "    var y: IThing? := null; "
        "    var t := C.Boom(); "              // throws BEFORE the marked transfer
        "    y := owned x; "                   // (never reached)
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    try var r := C.Take(move w); catch AppError => _ := 1; end; "
        "    return Cell.live; "               // 0 — disposed once on unwind
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

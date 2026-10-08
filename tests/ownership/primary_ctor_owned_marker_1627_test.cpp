// Regression tests for issue #1627: the §22.3 owned-call-site-marker check
// never resolved a class PRIMARY CONSTRUCTOR's parameter list, so passing a
// NAMED owned local (not a fresh producer) to an `owned` primary-ctor parameter
// silently DOUBLE-FREED.
//
// Root cause: SymbolCollection turns a primary-ctor parameter into a FieldSymbol
// (it lives on `TypeDecl::positionalFields`), never a ConstructorSymbol. The
// NewExpr owned-argument handlers in Ownership_Checks.cpp scanned only the
// resolved type's ConstructorSymbol members for a matching `ConstructorDecl`, so
// for a primary ctor they found none: the `owned` parameter was never treated as
// an owned sink. A named owned local `x` handed to it (`new Holder(x)`) was
// therefore never marked moved-from — the caller still believed it owned the
// value AND the constructed object's auto-disposer owned it too, so the value
// was disposed twice (double-free; valgrind: "Invalid free()").
//
// §11.1 / §22.10 (v0.180): "a primary-constructor parameter may be declared
// `owned`, and the same ownership-transfer rules as regular constructor
// parameters apply." §22.3 (v0.169): the `owned` call-site marker is REQUIRED
// when the argument is a named owned binding the call leaves moved-from —
// omitting it "is a compile error naming the fix". So `new Holder(x)` for an
// `owned` primary-ctor param must be REJECTED (write `new Holder(move x)`),
// exactly as it already was for a regular `owned` constructor parameter.
//
// Fix: a shared resolveCtorParams() helper falls back to the class primary
// constructor's `positionalFields` when no ConstructorDecl matches by arity, so
// the four NewExpr owned-classification sites recognize an `owned` primary-ctor
// parameter as an owned sink.
//
// AOT reproduction (build/bezelc + valgrind), pre-fix: `new Holder(x)` for an
// `owned` primary-ctor param compiled and valgrind reported "Invalid free()"
// (double-free) plus use-after-free reads. Post-fix: the same program is
// rejected with the §22.3 missing-owned-marker error; the marked form
// `new Holder(move x)` and a fresh producer `new Holder(new Res())` compile and
// dispose the resource exactly once (valgrind clean).
//
// Anti-over-reject surface (all ACCEPTED):
//   - `new Holder(move x)` — marked transfer to an owned primary-ctor param.
//   - `new Holder(new Res())` — fresh producer to an owned primary-ctor param.
//   - `new Sink(x)` where `Sink(r: Res)` has a NON-owned primary-ctor param —
//     a named owned local is a borrow here; no marker required, no double-free.
//   - ordinary non-owned primary-ctor params (`new Point(3, 4)`) unaffected.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §22.3 (v0.169) missing-owned-marker diagnostic — reportMissingOwnedMarker.
bool hasMissingOwnedMarkerError(const OwnershipResult& r,
                                const std::string& name = "") {
    const std::string needle = name.empty()
                                   ? "this call consumes `"
                                   : "this call consumes `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("§22.3") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// REJECTED — a named owned local passed UNMARKED to an `owned` PRIMARY-ctor
// parameter. §11.1 / §22.10 / §22.3 (#1627). This is the double-free the issue
// reports; before the fix it compiled and double-freed.
// =============================================================================

// LocalDecl-initializer form (`using var h := new Holder(x)`): exercises the
// recordMovesIn NewExpr handler. §22.3.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           unmarked_named_local_to_owned_primary_ctor_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder(owned r: Res) "
        "  private owned _r: Res := r; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var x := new Res(); "
        "    do using var h := new Holder(x); end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarkerError(r, "x"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Bare expression / discard statement form (`_ := new Holder(x)`): exercises the
// handleDischarge NewExpr handler — the exact `new Holder(x);` shape from the
// issue. §22.3.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           unmarked_named_local_discard_to_owned_primary_ctor_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder(owned r: Res) "
        "  private owned _r: Res := r; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var x := new Res(); "
        "    _ := new Holder(x); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarkerError(r, "x"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =============================================================================
// ACCEPTED — anti-over-reject surface.
// =============================================================================

// §22.3 marked transfer `new Holder(move x)`: the call-site marker announces
// the move; the primary-ctor param consumes `x`. No missing-marker error.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           marked_named_local_to_owned_primary_ctor_param_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder(owned r: Res) "
        "  private owned _r: Res := r; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var x := new Res(); "
        "    do using var h := new Holder(move x); end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarkerError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 fresh producer `new Holder(new Res())`: no live binding is invalidated,
// so no marker is written and none is required. Accepted.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           fresh_producer_to_owned_primary_ctor_param_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder(owned r: Res) "
        "  private owned _r: Res := r; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    do using var h := new Holder(new Res()); end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarkerError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Anti-over-reject: a NON-owned primary-ctor param (`Sink(r: Res)`). A named
// owned local passed unmarked is a BORROW, not a transfer — no marker is
// required and the fix must NOT reject it. §22.3 / §28.3.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           named_local_to_nonowned_primary_ctor_param_not_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Sink(r: Res) "
        "  public got: bool := true; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int "
        "    using var x := new Res(); "
        "    do using var s := new Sink(x); end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarkerError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Anti-over-reject: an ordinary non-owned primary constructor (`Point(x, y)`)
// with plain value params is entirely unaffected — no ownership diagnostics.
// §22.10.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           ordinary_nonowned_primary_ctor_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Point(x: int, y: int) "
        "  public px: int := x; "
        "  public py: int := y; "
        "end Point; "
        "class Program "
        "  public static method Main(): int "
        "    var p := new Point(3, 4); "
        "    return p.px + p.py; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarkerError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression anchor: a REGULAR (non-primary) `owned` constructor parameter still
// rejects an unmarked named owned local — the behavior the primary-ctor path now
// matches. §22.3.
BEZEL_TEST(primary_ctor_owned_marker_1627,
           regular_ctor_owned_param_still_rejects_unmarked) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class Holder "
        "  private owned _r: Res; "
        "  public constructor(owned r: Res) _r := r; end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var x := new Res(); "
        "    do using var h := new Holder(x); end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarkerError(r, "x"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

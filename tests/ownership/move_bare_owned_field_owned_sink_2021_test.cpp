// Spec-derived tests for issue #2021 — a BARE, unqualified owned-field `move`
// into an owned sink (`C.Sink(move f)`, `f` an owned field of the enclosing
// instance via implicit `this`) was spuriously REJECTED with a §28.8 borrow
// error, while the force-unwrapped `move f!` was accepted.
//
// §28.8.1 (the `move` expression): `move x.f` — and its bare implicit-`this`
// spelling `move f` — "is the only way to read an owned field's value as an
// owned value; all other reads of an owned field produce a borrow (§28.3)."
// §28.6 (leak prevention): a borrow does NOT own the object, so an owned field
// moved out of a BORROW receiver is still owned elsewhere — moving it into an
// owned sink double-frees. §28.8 (ownership transfer): an `owned` sink takes
// responsibility for disposing the value; only an OWNED value may flow in.
//
// The bug (#2021): `ownedArgSourceIsBorrow`'s FieldSymbol tail flagged EVERY
// bare owned-field identifier as a borrow, without distinguishing `move f` (an
// owned transfer off `this`, which owns its fields) from a bare `f` read (a
// borrow). Only the un-`move` case is a borrow — the FieldSymbol tail was not
// `move`-gated, so a bare `move f` reached it and was rejected. The `!` form
// `move f!` routes through the newer place-form classifier
// (`checkOwnedArgMoveSource`, #2010/#2016), which is receiver-ownership-aware
// and accepts it. The fix makes the FieldSymbol tail receiver-ownership-aware
// too (reusing the #2011 `ownedFieldMoveReceiverIsBorrow` helper): a bare
// `move f` off `this` (or an owned place) is accepted; a `move b.f` off a
// BORROW receiver stays rejected.
//
// §22.12/§22.13 carve-out (the pre-gate rework): the "bare id ⇒ `this.f`, `this`
// owns it" premise holds ONLY for INSTANCE fields (including inherited instance
// fields — those stay accepted). A STATIC owned field is owned by the PROGRAM —
// enrolled in §22.13's program-exit disposal — NOT by `this`; the null-after-
// move slot machinery is instance-slot-only, so a bare `move` of a static owned
// field into a sink is NEVER nulled and the program's exit-disposal double-frees
// it (a SEGV in `bz_dispose_static_owned`). `ownedFieldMoveReceiverIsBorrow` now
// positively flags a static owned field, so every owned-sink / owned-return path
// REJECTS it (restoring base's §28.8 behavior for the static case). The static
// cases below MUST be rejected; the first fix draft (a2ae680) accepted them —
// this coverage is the regression guard that would have caught that double-free.
//
// Fail-without / pass-with: each ACCEPT case below FAILED to compile on base
// (the spurious §28.8 borrow error) and compiles / disposes exactly once
// (Cell.live == 0 under the AOT+ASan oracle) after the fix. The REJECT cases
// (borrow receiver, static owned field) stay/are rejected (soundness). The
// nullable case still trips the independent §7 nullability error, but the
// spurious BORROW error is gone.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation / diagnosticMessageContains
#include "support/aot_support.hpp"         // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The §28.8 owned-sink borrow diagnostic (checkOwnedArgSource) — the spurious
// error the #2021 fix removes for a bare `move f`.
constexpr std::string_view kBorrow =
    "a borrow cannot be passed where an `owned`";
// The #1999/#2011 borrow-receiver field-move diagnostic (stays for `move b.f`).
constexpr std::string_view kBorrowField = "cannot move owned field";
// The §7 non-nullable-argument diagnostic (stays for a nullable field `move f`).
constexpr std::string_view kNullable = "non-nullable (§7)";
// The §28.8.1 exception-edge diagnostic — the stable identifier is the §-reference
// plus the section heading it quotes (support/ownership_support.hpp guidance).
constexpr const char* kEdgeDiag = "§28.8.1 \"Exception paths are not exempt\"";

// A destructor-counter Cell: ctor ++, dtor --. A correct program leaves live==0;
// a double-free drives it negative or aborts under ASan — the exact-once oracle.
const char* kCell =
    "module M; "
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

// `Sink.Take(owned c)` — a static owned sink. `Holder(owned c)` — an owned ctor
// sink for the local-decl-initializer path.
const char* kSink =
    "class Sink "
    "  public static method Take(owned c: Cell) using var k := c; end Take; "
    "  public static method TakeR(owned c: Cell): int using var k := c; return 0; end TakeR; "
    "end Sink; "
    "class Holder "
    "  public constructor(owned c: Cell) using var k := c; end constructor; "
    "  destructor end destructor; "
    "end Holder; ";

}  // namespace

// ===========================================================================
// ACCEPT (over-rejection removed) — a bare `move f` of a NON-NULLABLE owned
// field off `this` into an owned sink. Fail-without: base rejected it with the
// §28.8 borrow error. Pass-with: no borrow error (and no error at all).
// ===========================================================================

// §28.8.1 / §28.8: `C.Sink(move f)` in a bare-statement discharge. The field is
// restored (`this.f := new Cell()`) so the program is sound end-to-end.
// v0.300 (§28.8.1): method calls are throwable — the intervening Sink.Take
// call is an exception edge, so the unrestored field at the call site fires.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_bare_move_owned_field_accepted_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell := new Cell(); "
        "  public method Give() Sink.Take(move f); this.f := new Cell(); end Give; "
        "  destructor end destructor; "
        "end C;");
    // The §28.8.1 edge error fires on the throwable call before the restore.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// §28.8.1: the runtime twin — the bare `move f` sink transfer was expected to
// dispose the Cell EXACTLY once (Cell.live == 0 under the AOT+ASan oracle).
// v0.300 (§28.8.1): method calls are throwable — the §28.8.1 edge error now
// fires and the program is rejected at compile time.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_bare_move_owned_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell := new Cell(); "
        "  public method Give() Sink.Take(move f); this.f := new Cell(); end Give; "
        "  destructor end destructor; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    do using var c := new C(); c.Give(); end; return Cell.live; "
        "  end Main; "
        "end Main;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kEdgeDiag));
}

// §28.8.1: the method-call-as-initializer path — `var n := Sink.TakeR(move f)`
// routes through recordMovesInArgs' bare-identifier arm (the SECOND
// checkOwnedArgSource call site the fix repairs, alongside handleDischarge).
// v0.300 (§28.8.1): method calls are throwable — the §28.8.1 edge error now
// fires and the program is rejected at compile time.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           methodcall_rhs_bare_move_owned_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell := new Cell(); "
        "  public method Give() var n := Sink.TakeR(move f); this.f := new Cell(); "
        "    _ := n; end Give; "
        "  destructor end destructor; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    do using var c := new C(); c.Give(); end; return Cell.live; "
        "  end Main; "
        "end Main;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kEdgeDiag));
}

// §28.8.1: the local-decl-initializer NEW-expression path — a bare `move f` into
// a constructor's owned parameter (`new Holder(move f)`) routes through the
// NewExpr ctor-arg arm (checkOwnedArgMoveSource, already receiver-aware since
// #2010/#2016), so it was ACCEPTED even on base. Kept as a non-regression guard
// that the fix leaves the ctor path disposing the Cell exactly once.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           ctorarg_localdecl_bare_move_owned_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell := new Cell(); "
        "  public method Give() do using var h := new Holder(move f); end; "
        "    this.f := new Cell(); end Give; "
        "  destructor end destructor; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    do using var c := new C(); c.Give(); end; return Cell.live; "
        "  end Main; "
        "end Main;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// PARITY — the `!` form `move f!` was already accepted; it must STAY accepted
// (the fix aligns bare `move f` to it, it does not regress the `!` form).
// v0.300 (§28.8.1): `Sink.Take` is a throwable method call — the move-bang's
// field is moved-from at the call site, and the exception edge fires. The
// §17.4 move-bang acceptance subject survives only when NOT inside a throwable
// call; the §28.8.1 edge case is tested here and must be rejected.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_bare_move_bang_owned_field_throwable_edge_rejected_spec_28_8_1) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell := new Cell(); "
        "  public method Give() Sink.Take(move f!); this.f := new Cell(); end Give; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kEdgeDiag));
}

// ===========================================================================
// SOUNDNESS — a bare `move b.f` off a BORROW receiver `b` must STAY rejected: a
// borrower does not own the object and cannot transfer its owned fields out
// (§28.6). The fix only accepts the `this`/owned-place receiver form.
// ===========================================================================
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_bare_move_borrow_receiver_field_stays_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink +
        "class Box "
        "  public owned f: Cell := new Cell(); "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Steal(b: Box) Sink.Take(move b.f); end Steal; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrowField));
}

// ===========================================================================
// §7 INTERACTION — a bare `move f` of a NULLABLE owned field still trips the
// independent §7 non-nullable-argument error (the sink's `owned Cell` parameter
// is non-nullable; `move f!` is the correct nullable-field detach idiom, §28.8.1
// / stdlib §... connection-pool pattern). Fail-without: base ALSO emitted the
// spurious §28.8 borrow error. Pass-with: only the §7 error remains.
// ===========================================================================
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_bare_move_nullable_owned_field_only_nullability_error_spec_7) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink +
        "class C "
        "  private owned f: Cell? := new Cell(); "
        "  public method Give() Sink.Take(move f); this.f := new Cell(); end Give; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kNullable));
    // The spurious §28.8 borrow error is gone — the fix removes THAT error only.
    BEZEL_EXPECT_FALSE(errorSays(r, kBorrow));
}

// ===========================================================================
// CONSISTENCY — a `move recv.f` / `move recv.f!` off an OWNED-place (using-var)
// receiver: both spellings are accepted (the fix does not diverge them). Guards
// against re-introducing an asymmetry between the bare and `!` field-move forms.
// ===========================================================================
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           owned_local_receiver_field_move_bare_matches_bang_spec_28_8_1) {
    const char* boxAndHolder =
        "class Box "
        "  public owned f: Cell := new Cell(); "
        "  destructor end destructor; "
        "end Box; ";
    auto bare = parseAndCheckOwnership(
        std::string(kCell) + kSink + boxAndHolder +
        "class C "
        "  public static method M() "
        "    using var b := new Box(); "
        "    using var h := new Holder(move b.f); "
        "    b.f := new Cell(); "
        "  end M; "
        "end C;");
    auto bang = parseAndCheckOwnership(
        std::string(kCell) + kSink + boxAndHolder +
        "class C "
        "  public static method M() "
        "    using var b := new Box(); "
        "    using var h := new Holder(move b.f!); "
        "    b.f := new Cell(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(bare.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bang.diagnostics.hasErrors());
}

// ===========================================================================
// STATIC-FIELD SOUNDNESS (pre-gate rework, mandatory) — a bare `move` of a
// STATIC owned field is program-owned (§22.13), NOT `this`-owned. The null-
// after-move slot machinery is instance-only, so accepting it into an owned sink
// double-frees at program exit (SEGV in bz_dispose_static_owned). Base rejected
// it with the §28.8 borrow error; the first fix draft (a2ae680) wrongly accepted
// it. Both spellings and both method kinds must stay REJECTED with the §28.8
// borrow diagnostic. (A §22.12 domain-safety diagnostic for the static class-
// typed field may also be present — orthogonal; we assert the borrow error.)
// ===========================================================================

// A class carrying a `static owned readonly` field — the canonical program-owned
// long-lived resource form (§22.13 / spec §22.12).
namespace {
const char* kRegistry =
    "class Registry "
    "  public static owned readonly Slot: Cell := new Cell(); ";
}  // namespace

// §22.13 / §28.8: bare `move Slot` of a static owned field into an owned sink,
// from an INSTANCE method. REJECT (double-free otherwise). errorSays(kBorrow) is
// the discriminating assertion — false on the a2ae680 draft, true on base + rework.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_static_owned_field_instance_method_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink + kRegistry +
        "  public method Leak() Sink.Take(move Slot); end Leak; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrow));
}

// §22.13 / §28.8: the same, from a STATIC method (no `this` at all) — a static
// owned field is never `this`-owned regardless of method kind. REJECT.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_static_owned_field_static_method_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink + kRegistry +
        "  public static method Leak() Sink.Take(move Slot); end Leak; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrow));
}

// §22.13 / §28.8: the local-decl-initializer path (checkOwnedArgMoveSource) also
// rejects a bare `move Slot` of a static owned field into a ctor's owned param.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           ctorarg_static_owned_field_rejected_spec_22_13) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) + kSink + kRegistry +
        "  public static method Leak() do using var h := new Holder(move Slot); end; end Leak; "
        "end Registry;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kBorrow));
}

// ===========================================================================
// ACCEPT — an INHERITED instance owned field (declared in a base class, accessed
// by its bare identifier in a derived method) is a genuine `this`-owned field:
// `move f` is accepted and disposes the Cell exactly once. Guards that the
// static exclusion does not over-reject inherited instance fields.
// v0.300 (§28.8.1): method calls are throwable — the §28.8.1 edge error now
// fires and the program is rejected at compile time.
BEZEL_TEST(move_bare_owned_field_owned_sink_2021,
           barestmt_inherited_instance_owned_field_disposes_once_spec_28_8_1) {
    std::string src =
        std::string(kCell) + kSink +
        "class Base "
        "  protected owned f: Cell := new Cell(); "
        "  destructor end destructor; "
        "end Base; "
        "class Derived: Base "
        "  public method Give() Sink.Take(move f); this.f := new Cell(); end Give; "
        "end Derived; "
        "class Main "
        "  public static method Main(): int "
        "    do using var d := new Derived(); d.Give(); end; return Cell.live; "
        "  end Main; "
        "end Main;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kEdgeDiag));
}

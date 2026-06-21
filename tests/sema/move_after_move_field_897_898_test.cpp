// fix #897 #898 / §28.8.1: a `move` of an ALREADY-moved-from owned field must
// be diagnosed at the MOVE SITE.
//
// The path-sensitive field-restoration checker (#867) tracked the per-block
// `fieldMovedFrom_` facts but never CONSULTED them at a move site, so a second
// `move x.f` on an already-moved-from field was silently accepted — the field
// still held the original pointer, so the second move double-owned it:
//
//   * #897 (HIGH, crash): a straight-line double-move `move this._f; move
//     this._f;` compiled and, at scope end, disposed the same object twice — a
//     use-after-free (SIGSEGV).
//   * #898 (MED, under-reject): a loop body that moves the field but restores
//     it only conditionally — on the moved-from back-edge path the next
//     iteration's move operated on an already-moved-from field, undiagnosed.
//
// The fix mirrors the locals' read/move-after-move diagnostic (markMovedByName,
// which reports "was already moved out at this point (§28.3)") for fields: at
// each `move x.f` / `move this.f` / bare `move _f`, BEFORE marking the field
// moved-from, check the path-sensitive `fieldMovedFrom_` fact (#867) and report
// a §28.8.1 error if the field is already moved-from on the current path.
// Because the facts are path-sensitive and OR-merged over the loop back-edge,
// the move-site check catches #898's partial-restore loop too.
//
// These tests pin the rejection on both the bare and qualified forms (#825
// form-parity) and guard against over-rejecting a valid move-restore-move
// sequence or a loop that restores the field on every back-edge path.
// // per §28.8.1, #897 #898

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrapper: a class with a non-nullable owned Resource field, a no-op
// constructor, and a no-op destructor. Individual tests inline the method
// under scrutiny.
std::string withField(const std::string& method) {
    return std::string(
        "module M; "
        "class Resource "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Resource; "
        "class Owner "
        "  private owned _res: Resource; "
        "  public constructor() _res := new Resource(); end constructor; ") +
        method +
        " destructor end destructor; "
        "end Owner; "
        "class C public static method Main(): int return 0; end Main; end C;";
}

// True iff some emitted diagnostic mentions the move-site already-moved-from
// rule (the new §28.8.1 #897/#898 diagnostic, distinct from the at-exit
// "left in moved-from state" restoration diagnostic of #867).
bool hasMoveAfterMoveFieldError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is already moved-from at this point") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.8.1 / #897: straight-line double-move of a QUALIFIED owned field. The
// second `move this._res` operates on an already-moved-from field — there is no
// ownership to transfer; it double-owns the original object. Must be REJECTED.
// (Before the fix this compiled and crashed at runtime with a use-after-free.)
BEZEL_TEST(move_after_move_field_897_898, double_move_qualified_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method TakeTwice(): int "
        "  using var a := move this._res; "
        "  using var b := move this._res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end TakeTwice;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMoveAfterMoveFieldError(r));
}

// §28.8.1 / #897 (form-parity with #825): the BARE `move _res` form must reject
// a straight-line double-move identically to the qualified form.
BEZEL_TEST(move_after_move_field_897_898, double_move_bare_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method TakeTwice(): int "
        "  using var a := move _res; "
        "  using var b := move _res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end TakeTwice;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMoveAfterMoveFieldError(r));
}

// §28.8.1 / #897 (form-parity): a mixed qualified-then-bare double-move is the
// same operation on the same field; must reject.
BEZEL_TEST(move_after_move_field_897_898, double_move_mixed_forms_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method TakeTwice(): int "
        "  using var a := move this._res; "
        "  using var b := move _res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end TakeTwice;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMoveAfterMoveFieldError(r));
}

// §28.8.1 / #898: a loop body moves the field but restores it only on the `c`
// path. On the c == false back-edge the field stays moved-from, so the next
// iteration's `move this._source` operates on an already-moved-from field. The
// path-sensitive fact OR-merged over the back-edge makes the move-site check
// fire. Must be REJECTED.
BEZEL_TEST(move_after_move_field_897_898, loop_partial_restore_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method Test(cond: bool, c: bool): int "
        "  while cond do "
        "    using var old := move this._res; "
        "    if c then this._res := new Resource(); end; "
        "  end; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end Test;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMoveAfterMoveFieldError(r));
}

// §28.8.1 / #897 #898 (over-reject guard): move, RESTORE, then move again — the
// field was restored between the two moves, so the second move is valid. Must
// be ACCEPTED. A move-site check that ignored the intervening restore would
// wrongly reject this.
BEZEL_TEST(move_after_move_field_897_898, move_restore_move_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method MRM(): int "
        "  using var a := move this._res; "
        "  this._res := new Resource(); "
        "  using var b := move this._res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end MRM;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #897 #898 (over-reject guard, bare form): the bare `move _res`
// move-restore-move must be accepted identically.
BEZEL_TEST(move_after_move_field_897_898, move_restore_move_bare_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method MRM(): int "
        "  using var a := move _res; "
        "  this._res := new Resource(); "
        "  using var b := move _res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end MRM;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #898 (over-reject guard): a loop that moves AND UNCONDITIONALLY
// restores the field each iteration — the field is restored on every back-edge
// path, so the next iteration's move is valid. Must be ACCEPTED.
BEZEL_TEST(move_after_move_field_897_898, loop_unconditional_restore_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method Loop(cond: bool): int "
        "  while cond do "
        "    using var o := move this._res; "
        "    this._res := new Resource(); "
        "  end; "
        "  return 0; "
        "end Loop;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #897 #898 (over-reject guard): a single move followed by a restore
// and no second move is the canonical #825/#867 normal case. Must be ACCEPTED.
BEZEL_TEST(move_after_move_field_897_898, single_move_restore_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method Single(): int "
        "  using var a := move this._res; "
        "  this._res := new Resource(); "
        "  return 0; "
        "end Single;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

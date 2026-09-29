// Spec-derived tests for §9.1 / §9.4 (v0.236, issue #2309): the property
// auto-generated from a positional RECORD field is read-only — "Properties
// auto-generated from positional fields are read-only" (§9.4), and §9.1
// defines the surface ("Public read-only properties, one per positional
// field, with the same names"). Assignment to that property is a compile
// error; the pre-fix compiler accepted it and codegen wrote through the
// backing storage, producing a silent wrong-value miscompile (the issue's
// own repro: `r.X := 99` then `r.X` returned 99, the assigned value, not
// 1 the original).
//
// The fix lives in ExprTypecheck.cpp's `checkReadonlyAssignment`, the
// existing per-`AssignmentStmt` readonly check (the `readonly` modifier
// branch above the new one is unchanged and still covers class readonly
// fields per §22.8). The new branch matches when the target's symbol is a
// `FieldSymbol` with `posParam != nullptr` whose parent is a RECORD — the
// exact shape that §9.1's generator produces — and reports a diagnostic
// that names the spec section and points the user at `with` (§9.5) as
// the non-destructive replacement.
//
// The construction-window carve-out is NARROWER than the existing one
// above it: only the DECLARING record's constructor (so a combined-form
// record's body constructor can still initialize its positional fields,
// the §9.2 example) — a class C constructor cannot write to a
// positional record's property through `r` even though it is itself a
// constructor. The narrower check uses `currentType_ == fs->parent` to
// gate the exemption; the pre-fix broad `if (inConstructor_) return;`
// above ran before the new branch and would have allowed that false
// ACCEPT.
//
// The same read-only check is mirrored on the two sibling surfaces that
// ALSO write through the field — IncrementStmt (`r.X++` / `r.X--`, which
// desugars to a reassignment per §17.6) and the ref/out field operand
// (`ref r.X` / `out r.X`, the §22.3 aliasing surface). Both are
// documented below.
//
// Tests:
//   1. REJECT: direct positional-record property assignment (§9.1/§9.4)
//   2. REJECT: second positional-field (`r.Y := ...`) — not first-only
//   3. REJECT: combined-form record method body assignment
//   4. REJECT: cross-instance receiver (`r2.X := 99` from outside)
//   5. REJECT: compound assignment (`r.X += 99`)
//   6. REJECT: `Type.X` STATIC-receiver assignment
//   7. REJECT: a CLASS constructor writing to a positional record's
//      property (the over-broad `inConstructor_` carve-out the BLOCKING
//      issue named — without the narrow `currentType_ == fs->parent`
//      check, the prior broad carve-out silently allowed this)
//   8. REJECT: `r.X++` (IncrementStmt path — never reaches
//      checkReadonlyAssignment, so a separate read-only branch is needed)
//   9. REJECT: `ref r.X` (ref/out operand path — also bypasses the
//      AssignmentStmt check)
//  10. ACCEPT (control): a user-defined WRITABLE field on a named record
//      (the §9.4 / §9.2 distinction: body-declared fields may be mutable)
//  11. ACCEPT (control): a user-defined WRITABLE field on a CLASS
//  12. ACCEPT (control): a combined-form record's body constructor that
//      initializes a positional field — the narrow carve-out's reason
//      for existing
//   7. ACCEPT (control): a combined-form record's body constructor that
//      initializes a positional field — the §22.8 carve-out's parallel for
//      records, allowed by the `inConstructor_` early-return

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// =========================================================================
// §9.1 / §9.4 / #2309 — REJECTION SHAPES
// =========================================================================

// §9.1 / §9.4: a positional record's auto-generated property is read-only.
// Direct `r.X := 99` on `record R(X: int, Y: int)` is a compile error.
// Pre-fix: silently accepted; codegen wrote through and `r.X` returned 99.
BEZEL_TEST(record_positional_readonly_2309,
           direct_property_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    r.X := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "positional record"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "read-only"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§9.1"));
}

// §9.1 / §9.4: same shape through the second positional field — the check
// must not be hard-coded to the FIRST field. `r.Y := 99` is also a read-
// only-property write.
BEZEL_TEST(record_positional_readonly_2309,
           second_positional_property_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    r.Y := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `Y`"));
}

// §9.1 / §9.4: combined-form record — the same generated property is
// still read-only, even when the record body adds members. The user's
// method must not be able to write through it. Distinct from the body
// constructor carve-out (test #7 below).
BEZEL_TEST(record_positional_readonly_2309,
           combined_form_method_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int) "
        "  public method TrySet(v: int) X := v; end TrySet; "
        "end R; "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    r.TrySet(99); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// §9.1 / §9.4: assignment via a "second instance" receiver — `r2.X :=`
// where `r1` is a different positional record — must also reject. The
// readonly check is keyed on the field itself, not on which record owns
// the receiver.
BEZEL_TEST(record_positional_readonly_2309,
           cross_instance_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    var r1 := new R(1, 2); "
        "    var r2 := new R(3, 4); "
        "    r2.X := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// §9.1 / §9.4: compound assignment (`+=`) and `??=` are also rejected —
// they consume the AssignmentStmt path and reach the same check. Two
// flavours both refused; we name `??=` because it's the most likely
// "silent wrong-value" shape (assign-if-not-null reads from the field
// then writes back, both wrong if the field were ever writable).
BEZEL_TEST(record_positional_readonly_2309,
           compound_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    r.X += 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// §9.1 / §9.4: assignment through a STATIC receiver (`Type.X := ...`).
// The receiver-shape coverage is identical to the existing §22.8 readonly
// check — `checkReadonlyAssignment` resolves the target's member symbol
// directly, so every receiver shape (`this.f`, `c.f`, `Type.f`, bare
// `f`) is covered. The check is keyed on `fs->posParam && fs->parent is
// Record`, so `R.X := 99` reaches the same branch as `r.X := 99`.
BEZEL_TEST(record_positional_readonly_2309,
           static_receiver_assignment_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    R.X := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// BLOCKING (over-broad construction-window guard): a class C's
// CONSTRUCTOR must NOT be able to write to a positional record's
// property. The pre-fix `if (inConstructor_) return;` ran before the
// new positional-record check and returned early for ANY constructor
// body — so `class C { constructor() { var r := new R(1, 2); r.X := 5;
// }}` would have been silently accepted and codegen would have written
// through `r.X`'s backing storage. The narrower carve-out
// (`currentType_ == fs->parent` IN ADDITION to `inConstructor_`) keeps
// the §9.2 combined-form constructor case working while closing this
// false ACCEPT.
BEZEL_TEST(record_positional_readonly_2309,
           class_constructor_writing_to_record_property_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class C "
        "  public constructor() "
        "    var r := new R(1, 2); "
        "    r.X := 5; "
        "  end constructor; "
        "end C; "
        "class Program "
        "  public static method Main() "
        "    var c := new C(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// BLOCKING adjacent: a class C's METHOD body must not be able to write
// to a positional record's property either. (This case was already
// rejected by the existing `readonly` modifier branch's `if (inConstructor_)
// return;` NOT firing — methods don't set `inConstructor_` — so this
// rejection was already in place, but it's the second half of the
// over-broad-carve-out story and worth pinning explicitly.)
BEZEL_TEST(record_positional_readonly_2309,
           class_method_writing_to_record_property_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class C "
        "  public method TryMutate() "
        "    var r := new R(1, 2); "
        "    r.X := 5; "
        "  end TryMutate; "
        "end C; "
        "class Program "
        "  public static method Main() "
        "    var c := new C(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// §9.1 / §9.4 / §17.6: `r.X++` (and `r.X--`) on a positional record's
// auto-generated property — IncrementStmt desugars to a reassignment
// (§17.6), so it is exactly the same defect as `r.X := …` on a
// different path. The IncrementStmt handler never reaches
// checkReadonlyAssignment, so a parallel read-only check is needed.
// Pre-fix: silently accepted; codegen wrote the increment through the
// backing storage, `r.X` returned the incremented value.
BEZEL_TEST(record_positional_readonly_2309,
           increment_on_positional_property_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    r.X++; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "cannot increment/decrement"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
}

// §9.1 / §9.4 / §22.3: `ref r.X` (and `out r.X`) on a positional
// record's auto-generated property. The ref/out operand check has its
// own path (`checkRefOutFieldOperand`) and only looks at the `readonly`
// modifier — the positional-record case has no modifier, so without
// the new check `ref r.X` was silently accepted and codegen aliased a
// write through the read-only backing storage.
BEZEL_TEST(record_positional_readonly_2309,
           ref_operand_on_positional_property_rejected_spec_9_1_issue_2309) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int); "
        "class Sink "
        "  public static method TakeByRef(ref x: int) x := x + 1; end TakeByRef; "
        "end Sink; "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "    Sink.TakeByRef(ref r.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "ref"));
}

// =========================================================================
// §9.4 / §9.2 — ACCEPT CONTROLS (the rejection must not over-fire)
// =========================================================================

// §9.2 / §9.4: a user-defined writable field on a NAMED record (one with
// a body — `record R { public X: int := 0; }`) — the §9.4 read-only rule
// is on "properties auto-generated from positional fields", and body-
// declared fields are explicit user storage. Assignment MUST still be
// accepted. This is the canonical regression guard: a fix that swallows
// every record-field write would break this case.
BEZEL_TEST(record_positional_readonly_2309,
           named_record_user_field_assignment_accepted_spec_9_2) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public X: int := 0; "
        "end R; "
        "class Program "
        "  public static method Main() "
        "    var r := new R(); "
        "    r.X := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11 / §22: a user-defined writable field on a CLASS — assignment must
// be accepted. The new check explicitly guards on `parent->kind == Record`
// (and is skipped for classes entirely), so class field writes are
// untouched.
BEZEL_TEST(record_positional_readonly_2309,
           class_user_field_assignment_accepted_spec_22) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public X: int := 0; "
        "end C; "
        "class Program "
        "  public static method Main() "
        "    var c := new C(); "
        "    c.X := 99; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.1 / §22.8 (the construction-window carve-out, parallel to the
// `readonly` modifier's own carve-out above): a combined-form record's
// user-declared body constructor may initialize the positional field
// (e.g. `Width := width` in the §9.2 example). `inConstructor_` is true
// during `ConstructorDecl` typecheck so the new branch early-returns
// alongside the `readonly` modifier branch — both exempt constructor
// writes. Without this guard, every combined-form record that
// initializes its positional fields in a constructor would fail to
// compile.
//
// FLIPPED for fix #5132 (§9.3 / §22.6, v0.308). Under the v0.307 narrow
// predicate the `record R(X, Y) + constructor(x, y)` shape was admitted
// (case-sensitive name comparison made it not-an-exact-duplicate). Under
// §22.6 overload identity (names do not distinguish), the body ctor and
// the primary are indistinguishable, and §9.3 REJECTS the declaration.
// The construction-window carve-out this row originally pinned is now
// unobservable (the body ctor never compiles), so the row is flipped to
// a §9.3 reject oracle.
BEZEL_TEST(record_positional_readonly_2309,
           combined_form_constructor_init_rejected_spec_9_3) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(x: int, y: int) "
        "    X := x; "
        "    Y := y; "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main() "
        "    var r := new R(1, 2); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "\xC2\xA7""9.3"));
}

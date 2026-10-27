// Spec-derived regression tests for issue #7103 — the consume-site freeze
// (§28.3 K5.2 / §28.7 *Overwrite while borrowed*, v0.343) did not fire for an
// `Object`-typed local.
//
// §28.7 (v0.343), verbatim: "Reassigning a plain-`var` owned **local** while a
// live local borrow of its current value exists is rejected by the consume-site
// freeze (K5.2; §28.3) — the implicit disposal would silently invalidate the
// borrow."
//
// §28.3 *Ownership state of locals* (v0.343), the paragraph "A runtime-managed
// RHS into a classified local": a runtime-managed RHS (a box) stored into a
// CLASSIFIED local "runs the displacement the LHS's state prescribes in the
// list above — an owned value disposed per drop-on-overwrite (§28.7),
// **subject to the consume-site freeze**".
//
// §28.3 *Producers* (v0.343), the row "Local-variable read `x` where `x` is
// owned": in a **borrowing position** "`x` retains ownership and the value is
// borrowed for the consuming operation" — the READER's static type is not part
// of the rule.
//
// §28.3 *Ownership state of locals* (v0.343): a local in the OWNED state holds
// a CLASSIFIED value; the `unclassified` state (the one that "carries no
// obligation and no source, so a read of it is no borrow") is the state of a
// local holding a RUNTIME-MANAGED value — a box, a `string`, a function value.
//
// §28.3 K1.5 (v0.343): "Reads of named bindings lend unless the position
// consumes (K3): an unmarked read of an owned binding is a borrow of the
// binding", while "Reads of storage lend: a field, property, indexer, or slot
// read is a borrow whose source is the storage's holder".
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
// The ownership pass's borrow-vs-copy gate read the `Object` PRIMITIVE kind as
// a value type — the compiler's OWN comment on that gate calls `string` and
// `Object` "runtime-managed reference types whose lifetime is not tied to any
// single owner" (that is the compiler's sentence, not the spec's: §28.9's
// runtime-managed tier is `string`, `Type`, boxed value types and
// function-typed values, and `Object` is not on it) — so an `Object`-typed
// read of an OWNED local was never recorded as a borrow. The consume-site
// freeze consults the recorded borrow sources, so it could not see the holder.
// The two programs below differ in exactly ONE TOKEN — the declared type of
// `o` — and that token decided whether the program was REJECTED or read freed
// memory:
//
//   var o: Object := new Res(1);   -- REJECTED (row 1; was ACCEPTED)
//   var o := new Res(1);           -- REJECTED (row 5; always was)
//
// With the fix, the `Object`-typed spelling is rejected with the SAME
// diagnostic shape as the class-typed one.
//
// The rule is a REJECTION, not a "make it safe" change: the program that reads
// freed memory must be rejected at compile time. Every row below therefore
// asserts the DIAGNOSTIC — never "the program did not crash", which would be
// vacuous for a use-after-free (the crash may or may not happen).

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"       // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A class with a readable member, so the borrowed instance is actually READ
// after the overwrite (a use-after-free shape, not a dead borrow).
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Main(): int ") +
        body +
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The freeze diagnostic's shape, as the class-typed control produces it.
constexpr std::string_view kFreezeHead = "cannot consume `o` here";
constexpr std::string_view kFreezeHolder = "borrow held by `p`";
constexpr std::string_view kFreezeRule = "consume-site freeze";

// THE ARMED SENTINEL's known positive — a §8.6 boxing leak, loop-amplified so
// the boxes are unreachable at exit (a one-shot leak can still be reachable
// from a live stack slot and pass on both sides). Proven positive by
// tests/support/aot_leak_detection_4638_test.cpp.
constexpr const char* kLeakPositive =
    "module M; class C public static method Main(): int "
    "  var a: Array<Object> := [1, 2]; "
    "  var hits: int := 0; "
    "  for var i in 1..500 do "
    "    if a.Contains(i) then hits := hits + 1; end; "
    "  end; "
    "  return hits; "
    "end Main; end C;";

// THE ARMED SENTINEL's control — byte-for-byte the same program with
// `Array<int>`: no boxing, so no box to leak.
constexpr const char* kCleanControl =
    "module M; class C public static method Main(): int "
    "  var a: Array<int> := [1, 2]; "
    "  var hits: int := 0; "
    "  for var i in 1..500 do "
    "    if a.Contains(i) then hits := hits + 1; end; "
    "  end; "
    "  return hits; "
    "end Main; end C;";

} // namespace

// ---------------------------------------------------------------------------
// (1) THE REPRO — an `Object`-typed local overwritten while a live borrow of
//     its instance exists. REJECTED by the consume-site freeze.
// ---------------------------------------------------------------------------

// §28.7: `var o: Object := new Res(1); var p: Object := o; o := 42;` — the
// store displaces `o`'s owned instance (§28.3 *State match*: a runtime-managed
// RHS into a classified local "runs the displacement the LHS's state
// prescribes ... an owned value disposed per drop-on-overwrite (§28.7),
// subject to the consume-site freeze"), and `p` is a live local borrow of
// that instance (§28.3 *Producers*: a read of an owned local in a borrowing
// position is a borrow). The overwrite would silently invalidate the borrow,
// so the program is REJECTED.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_local_overwrite_while_borrowed_rejected_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    o := 42; "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeRule));
}

// The same rejection through a MARKED TRANSFER consume site (§28.3's consume
// table: a `move` is a consume of the binding's current value) — `using var
// y := move o;` — so the freeze is not narrowly a reassignment rule.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_local_marked_transfer_while_borrowed_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    using var y := move o; "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// The INFERRED-TYPE twin: `var p := o;` — the holder's type is inferred, and
// the freeze must not depend on the holder's spelling either.
BEZEL_TEST(consume_site_freeze_object_typed,
           inferred_holder_type_overwrite_while_borrowed_rejected_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var o: Object := new Res(1); "
        "    var p := o; "
        "    o := 42; "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ---------------------------------------------------------------------------
// (2) THE CONTROLS — ACCEPTED. These pin the fix's PRECISION: the fix must
//     not reject programs the spec admits.
// ---------------------------------------------------------------------------

// §28.3 *Ownership state of locals*: an `Object`-typed local holding a BOX is
// in the `unclassified` state — it "carries no obligation and no source, so a
// read of it is no borrow" — and the box is disposed by nothing. So overwriting
// it displaces no owned value, and the §28.7 freeze has nothing to fire on.
// ACCEPTED.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_local_holding_box_overwrite_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(mod(
        "    var o: Object := 42; "
        "    var p: Object := o; "
        "    o := 7; "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3's freeze liveness: "a borrow whose only uses precede the consume is
// not live". `p` is never read after the overwrite, so nothing can dangle —
// ACCEPTED (the borrow is dead at the consume site).
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_borrow_without_later_use_accepted_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    o := 42; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The §28.3 must-use lint still runs over the (accepted) program — the
    // rejection this file is about is the FREEZE, not the auto-drop lint.
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "o"));
}

// ---------------------------------------------------------------------------
// (3) THE PRE-EXISTING CONTROL — the class-typed twin, rejected before this
//     fix and after it. It is the shape the `Object`-typed spelling now
//     matches, and it must keep its diagnostic.
// ---------------------------------------------------------------------------

BEZEL_TEST(consume_site_freeze_object_typed,
           class_typed_local_overwrite_while_borrowed_rejected_control_spec_28_7) {
    auto r = parseAndCheckOwnership(mod(
        "    var o := new Res(1); "
        "    var p: Object := o; "
        "    o := new Res(2); "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
}

// ---------------------------------------------------------------------------
// (3b) THE SECOND AND THIRD FLIPS — the fix sets the borrow SOURCE for an
//      `Object`-typed holder, and the borrow-escape-on-return check reads that
//      same field. So the fix's effect is WIDER than "the freeze now fires":
//      it also closes the borrow-escape-on-return hole for the same spelling.
//      Both flips were ACCEPTED on the base (the class-typed twins of both
//      are rejected there, so the `Object`-typed spelling was an
//      under-rejection — the safe direction to close).
// ---------------------------------------------------------------------------

// FLIP 3 — the alias is first stored into a field, then the source is
// overwritten. §28.7: the store displaces `o`'s owned instance while `p` —
// still live — borrows it, so the freeze rejects. The field store does not
// change the verdict: the borrow is still live at the overwrite.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_alias_stored_into_field_then_overwrite_rejected_spec_28_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  private _f: Object; "
        "  public constructor() _f := null; end constructor; "
        "  public method Set(x: Object) _f := x; end Set; "
        "end Holder; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    h.Set(p); "
        "    o := 42; "
        "    var n := p.GetHashCode(); "
        "    _ := n; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHead));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeHolder));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFreezeRule));
}

// FLIP 4 — the alias is RETURNED. This fires a DIFFERENT rule: the
// borrow-escape-on-return check (§28.3 / §9.7's borrow-lifetime rule), not
// the consume-site freeze — the value would outlive the scope-bound local it
// borrows. Pinned here so the flip can never be mistaken for the freeze's.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_alias_returned_is_a_borrow_escape_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Get(): Object "
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    return p; "
        "  end Get; "
        "  public static method Main(): int "
        "    var x := C.Get(); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned `p` carries a borrow of `o` whose source is disposed at "
           "scope end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "borrow-lifetime rule"));
}

// ---------------------------------------------------------------------------
// (3c) THE POSITIVE CONTROLS for the escape-on-return arm. These three are
//      the ADMIT side of the same check the flip above closes: without them a
//      later change could over-reject a legal return and no row would notice.
//      All three are accepted on the base AND with the fix (measured).
//
//      WHAT THESE ROWS PIN — and what they do NOT. Each row pins the
//      escape-on-return ARM's verdict on its shape (this arm must not fire),
//      and nothing else. Each of the three programs ALSO sits on a SEPARATE,
//      PRE-EXISTING compiler gap, so they are not claims that the programs
//      are spec-legal as a whole:
//
//        The compiler models `Object` as a PRIMITIVE value type
//        (sema::TypeRef::Kind::Primitive), so every ownership rule that
//        asks "is this a class type?" misses it. §3.1 says the opposite:
//        "`Object` is a class, named `Object` or, qualified,
//        `System.Object`". Three consequences, all measured on this tree:
//
//          (a) a STATIC method whose declared return type is `Object` is
//              ACCEPTED, while §28.3 *Factory methods and sourceless
//              returns* rejects a static declaration whose declared return
//              type is a class or interface type ("names no one responsible
//              for the result") — the compiler enforces that rule for `Res`
//              and misses it for `Object`;
//          (b) `owned Object` is REJECTED ("`owned` is not admissible on a
//              return type of type a primitive value type"), while §28.1
//              admits `owned` on a class type and `Object` is a class;
//          (c) a fresh owned value returned through a plain `Object` return
//              is not rejected (§28.6's fresh-value rule).
//
//        All three of the programs below declare a static method with a
//        plain `Object` return, so (a) is what admits them. If that gap is
//        closed later, these three rows must be revisited — they will fail
//        on the DECLARATION-SITE rule, not on the escape arm this fix
//        touches. Closing that gap is OUT OF SCOPE for #7103 (the
//        consume-site freeze); it is reported to the coordinator as an
//        adjacent finding.
// ---------------------------------------------------------------------------

// CONTROL 1 — the borrow's source is a PARAMETER, whose source is the
// CALLER: it outlives this frame, so returning it is legal (§28.3's
// producers table, "Parameter `p` (no modifier) | Borrowed (source:
// caller)").
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_alias_of_a_parameter_return_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Fwd(src: Object): Object "
        "    return src; "
        "  end Fwd; "
        "  public static method Main(): int "
        "    var o: Object := new Res(1); "
        "    var p: Object := C.Fwd(o); "
        "    var n := p.GetHashCode(); "
        "    _ := n; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 2 — a MARKED TRANSFER moves the owned value out; nothing borrows
// it, so the return is legal. Per §28.3 K1.5, a `move` result is a FRESH
// PRODUCTION and is therefore owned ("Fresh productions are owned: `new`,
// reference-typed collection literals, `owned`-returning calls, `move`
// results, owned operator results") — so the returned value carries no
// borrow at all, and the escape-on-return arm has nothing to fire on. (The
// consume table's row K3.4 is the marked BINDING/ASSIGNMENT transfer, not a
// return; the `return` position is row K3.2.)
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_move_transfer_return_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Make(): Object "
        "    var o: Object := new Res(1); "
        "    return move o; "
        "  end Make; "
        "  public static method Main(): int "
        "    var p: Object := C.Make(); "
        "    var n := p.GetHashCode(); "
        "    _ := n; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 3 — the returned value is a FRESH producer, not a borrow of a
// local: nothing is borrowed, so nothing can outlive anything.
BEZEL_TEST(consume_site_freeze_object_typed,
           object_typed_fresh_producer_return_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method N(): int return _v; end N; "
        "end Res; "
        "class C "
        "  public static method Make(): Object "
        "    return new Res(2); "
        "  end Make; "
        "  public static method Main(): int "
        "    var p: Object := C.Make(); "
        "    var n := p.GetHashCode(); "
        "    _ := n; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (4) THE AOT DRIVER — the repro is rejected on the REAL shipping path, and
//     the rejection is the DIAGNOSTIC (not "it did not crash").
// ---------------------------------------------------------------------------

// The whole point of this row: the program is REJECTED AT COMPILE TIME, so
// there is no generated program to run and no use-after-free to observe. The
// assertion is on the diagnostic text the driver prints; a "did not crash"
// assertion would be vacuous here (the crash may or may not happen).
BEZEL_TEST(consume_site_freeze_object_typed,
           uaf_repro_is_rejected_by_the_aot_driver_spec_28_7) {
    auto r = runAot(mod(
        "    var o: Object := new Res(1); "
        "    var p: Object := o; "
        "    o := 42; "
        "    var h := p.GetHashCode(); "
        "    _ := h; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "cannot consume"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "borrow held by"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "consume-site freeze"));
}

// ---------------------------------------------------------------------------
// (5) THE ARMED SENTINEL — the ASan/LSan leg this lane's evidence rests on is
//     ARMED: a deliberately leaking program IS reported, and the leak-free
//     control of the same shape is NOT. Without this, "the sanitizer found
//     nothing" would be an unfalsifiable claim.
// ---------------------------------------------------------------------------

// The SENTINEL: a known-positive leak (500 §8.6 boxes) must be reported by
// LSan through the same ASan-instrumented leg that would catch a
// use-after-free. `runAotChecked` enables generated-code ASan AND
// LeakSanitizer's exit-time check.
BEZEL_TEST(consume_site_freeze_object_typed,
           asan_sentinel_reports_a_deliberate_leak_spec_28_2) {
    auto r = runAotChecked(kLeakPositive);
    // A box without the C toolchain SKIPs the whole leg; an unarmed oracle
    // must not report a green sentinel.
    if (bezel::test::checkedToolchainSkipped(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "LeakSanitizer"));
}

// The sentinel's CONTROL: the same program with no boxing leaks nothing, so
// the leg stays quiet — which is what makes row 5 a measurement rather than a
// tautology.
BEZEL_TEST(consume_site_freeze_object_typed,
           asan_sentinel_control_leak_free_program_stays_quiet_spec_28_2) {
    auto r = runAotChecked(kCleanControl);
    if (bezel::test::checkedToolchainSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

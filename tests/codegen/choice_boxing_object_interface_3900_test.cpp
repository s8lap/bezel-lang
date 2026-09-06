// fix #3900 — Choice never boxes to Object/interface: `isBoxableValueType`
// had no `Choice` arm, so §35.8 *Boxing and interfaces* choice-to-`Object`
// boxing was unimplemented (spec §35.8/§8.6, v0.294).
//
// §35.8, verbatim: "Boxing follows the classification, each arm riding an
//   existing §8.6 rule: — Value-like choices box per the general rule; the
//   box is runtime-managed. — Borrow-bearing choices box under §8.6's
//   borrow-bearing-record rules verbatim ... — Owning choices cannot be
//   boxed — §8.6's move-only rule, word for word: boxing copies, move-only
//   forbids copying."
// §8.6, verbatim: "When a value type is used where `Object` (or an
//   interface implemented by the value type) is expected, the value is
//   boxed: a heap-allocated wrapper holds the value."
//
// ── DISCOVERED DURING (fix #3893) ────────────────────────────────────────
// #3893's own audit found that `s.Equals(objectHoldingEqualShape)` printing
// `True` pre-#3893 was TWO defects cancelling: `boxForTarget`'s final gate
// (`if (!isBoxableValueType(srcTy) && ...) return v;`) silently returned the
// UNBOXED raw `Shape` struct for an `Object`-declared local, and the pre-fix
// choice `.Equals` arm blindly loaded `other` through `Shape`'s own struct
// type — which happened to read back the right bytes only because `o` was
// never actually a box. Post-#3893 (this branch's base) that coincidence is
// gone and the path cleanly DECLINES (the pre-existing #2434 "failed to
// lower, degraded to a zero-initialized value" internal note) instead of
// reading garbage — safer, but still not the §35.8-mandated boxing.
//
// ── THE FIX ───────────────────────────────────────────────────────────────
// `isBoxableValueType` (LlvmBackend_Setup.cpp) now admits a NON-GENERIC
// `Choice` declaration, exactly as it already admits Record/Enum. Codegen
// does NOT re-derive the owning-vs-boxable classification (CLAUDE.md §6): an
// owning-classified choice's conversion to `Object`/an interface is already
// rejected at every conversion site by `ExprTC::checkAssignable` via
// `common::isMoveOnlyValueType` (fix #2912/#3775, unrelated to this fix, and
// unmodified by it) — confirmed on this tree, T4 below — so by construction a
// Choice `TypeRef` reaching `isBoxableValueType` is never owning. Boxing
// then reuses the EXISTING Record/Enum descriptor + box-vtable machinery
// (`setupBoxDescriptors`'s Pass A-D), widened to admit `Choice` — §35.2 "a
// choice body admits exactly what an enum body admits" means the interface-
// itable construction that already serves Enum needs no new code to also
// serve Choice. The box's Equals/GetHashCode/ToString slots dispatch through
// the SAME `emitChoiceEquality`/`emitChoiceHashCode`/`emitChoiceToString`
// the unboxed `.Equals`/`.GetHashCode`/`.ToString()` path already used
// (§35.7's synthesized tag+active-payload forms) — the discriminant (tag) is
// therefore part of the comparison by construction, not merely by the
// struct layout (T1's diffCase row pins this: two DIFFERENT cases sharing
// an identical int payload must still compare unequal).
//
// A choice that is neither auto-hashable (§35.7) nor declares its own
// `ToString()`/`Equals`/`GetHashCode` override has no §35.7-defined default
// for those members — the unboxed direct-call surface makes this a compile
// ERROR (§9.2's use-site rule), which the boxed dynamic-dispatch surface
// cannot replicate (the static receiver type is `Object`/an interface, so
// there is nothing to reject at compile time). Declined-with-reasons here:
// `ToString` falls back to the qualified type name (the same sound default
// §8.5 gives a non-auto-hashable body record); `Equals`/`GetHashCode` fall
// back to the ordinary §8.3 Object default (reference/box identity) rather
// than guessing at a comparison the direct-call surface itself never allows.
//
// ── SCOPE / WHAT IS NOT COVERED ───────────────────────────────────────────
// A GENERIC choice (`Result<T, E>` and friends) is deliberately EXCLUDED
// from `isBoxableValueType` here (unchanged from pre-fix — it still declines
// via the pre-existing #2434 path, not a new regression): the shared box
// vtable this fix builds is emitted ONCE with no active generic
// substitution, so it would see only the TEMPLATE-erased struct layout —
// correct for a generic RECORD only because #2176's `specializedBoxVtableFor`
// mints a per-instantiation vtable over the concrete layout instead (a
// record-only mechanism). Admitting a generic choice without an analogous
// per-instantiation vtable would silently compare/hash/render a boxed
// `Result<string, E>` through the erased (narrower) template layout — an
// out-of-bounds read, not merely a wrong answer — so it is left declined and
// filed as a follow-up mirroring #2176 for choices (issue in the fix commit
// body). Likewise left declined: calling a user-declared homogeneous
// `Equals(T)`/`GetHashCode` pair (§35.7's mutually-exclusive "declares user
// equality" rescue, only reachable by a NON-auto-hashable choice) through
// the BOXED dispatch path — the boxed slot for that case falls back to the
// ordinary reference-identity default instead (sound, never crashes; a
// narrower gap than the auto-hashable case this fix closes).
//
// ── WHAT EACH ROW IS FOR ───────────────────────────────────────────────────
//   T1  the audit's repro, generalized: boxing genuinely happens (not the
//       pre-#3893 unboxed-struct coincidence) AND the discriminant survives
//       boxing — `Circle(1)` and `Square(1)` share an identical `int`
//       payload but must compare UNEQUAL once both are boxed `Object`s.
//   T2  a boxed choice carries real type identity (`o is Shape`) and renders
//       the §35.7 synthesized form through the box's ToString slot.
//   T3  the boxed and unboxed GetHashCode paths (two independently-built
//       thunks, `emitChoiceHashCode` called from two different call sites)
//       agree for equal values — §8.3's Equals/GetHashCode consistency
//       contract, now that GetHashCode is reachable through Object too.
//   T4  REGRESSION GUARD: an owning-classified choice's conversion to
//       `Object` is still rejected at compile time (§8.6/§35.8) — confirms
//       this fix's `isBoxableValueType` widening does not disturb the
//       pre-existing #2912/#3775 move-only rejection it depends on.
//   T5  OWNERSHIP CONTROL (borrow-bearing choice, §35.8's second bullet):
//       loop-amplified 100x, under ASan/LSan. A choice holding an unmarked
//       (borrowed) class-typed payload is boxed, type-tested (a §8.6
//       non-escaping read so the box itself is cleanly released, isolating
//       the payload-ownership question from the pre-existing conservative
//       "call-argument escape" leak the record/enum box-release walker
//       already accepts for OTHER shapes — verified independently, not
//       exercised by this row). A clean ASan/LSan pass here means boxing
//       neither duplicates the borrowed `Res3900E` (no double-free when its
//       own scope ends) nor drops it (the summed `Get()` values are right).
//       ⚠ This row DOES flip on revert (see FAIL-WITHOUT below) — pre-fix,
//       `o is Box3900E` on the raw (unboxed) struct answers wrong, so `total`
//       never reaches 700.
//   T6  THE DECLINED-DEFAULT BRANCH (§35.7's "not auto-hashable" case, the
//       borrow-bearing `Box3900F`/§8.3 Object default `boxDefaultEqualsThunk`/
//       `boxDefaultHashCodeThunk`, and the qualified-name `ToString` fallback)
//       — UNEXERCISED by T1-T5, all of which use an auto-hashable choice.
//       Pins: `ToString()` on a genuinely-boxed non-auto-hashable choice
//       renders the qualified type name (not a crash, not the §35.7 form);
//       `Equals` is reference/box identity (self-equal, two separately-boxed
//       equal-payload values UNEQUAL — the opposite answer T1's auto-hashable
//       case gives, which is the point); `GetHashCode` is stable across two
//       calls on the SAME box (the §8.3 self-consistency a hash identity
//       needs, without asserting cross-object inequality, which is an
//       implementation-defined non-guarantee).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
// Native-ASan-in-a-lane-worktree is vacuous (returns a synthetic SKIP
// diagnostic rather than a real run) — mirrors the identical local helper in
// arg_position_box_release_2952_test.cpp; only the gate tree's multi-process
// run makes this row's ASan leg load-bearing.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}
}  // namespace

// ── T1 — the discriminant-preservation repro (§35.7/§35.8). Pre-fix: both
// rows below never reach a real answer — `eqSameCase` prints `False` (the
// #2434 decline this branch's base, post-#3893, produces), and `eqDiffCase`
// would ALSO print `False` for the wrong reason if the boxing were naive
// (e.g. a raw byte comparison of the struct with no tag check) rather than
// for the right one (different active case). Asserting the exact two-line
// stdout catches both directions.
BEZEL_TEST(codegen_choice_boxing_object_interface_3900,
           value_like_choice_boxes_to_object_with_discriminant_preserved_spec_35_7_35_8) {
    auto r = runAot(
        "module M3900A; "
        "choice Shape3900A Circle(radius: int), Square(side: int); "
        "end Shape3900A; "
        "class C "
        "  public static method Main(): int "
        "    var s: Shape3900A := Shape3900A.Circle(1); "
        "    var o: Object := Shape3900A.Circle(1); "
        "    var diffCase: Object := Shape3900A.Square(1); "
        "    Console.WriteLine(\"eqSameCase \" + s.Equals(o).ToString()); "
        "    Console.WriteLine(\"eqDiffCase \" "
        "      + s.Equals(diffCase).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("eqSameCase True\neqDiffCase False\n"));
}

// ── T2 — real box identity + §35.7 ToString via the box's slot (§8.2/§8.5).
BEZEL_TEST(codegen_choice_boxing_object_interface_3900,
           boxed_choice_type_test_and_tostring_spec_8_2_8_5_35_7) {
    auto r = runAot(
        "module M3900B; "
        "choice Shape3900B Circle(radius: int), Square(side: int); "
        "end Shape3900B; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := Shape3900B.Circle(7); "
        "    Console.WriteLine(\"isShape \" + (o is Shape3900B).ToString()); "
        "    Console.WriteLine(\"str \" + o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(
        r.stdoutText,
        std::string("isShape True\nstr Shape3900B.Circle(radius: 7)\n"));
}

// ── T3 — §8.3 Equals/GetHashCode consistency across the boxed/unboxed split.
BEZEL_TEST(codegen_choice_boxing_object_interface_3900,
           boxed_and_unboxed_gethashcode_agree_spec_8_3_35_7) {
    auto r = runAot(
        "module M3900C; "
        "choice Shape3900C Circle(radius: int), Square(side: int); "
        "end Shape3900C; "
        "class C "
        "  public static method Main(): int "
        "    var s: Shape3900C := Shape3900C.Circle(1); "
        "    var o: Object := Shape3900C.Circle(1); "
        "    Console.WriteLine(\"hashEq \" "
        "      + (s.GetHashCode() = o.GetHashCode()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hashEq True\n"));
}

// ── T4 — REGRESSION GUARD: an owning choice still cannot box (§8.6/§35.8),
// confirming this fix's isBoxableValueType widening does not disturb the
// pre-existing (#2912/#3775) move-only rejection it relies on.
BEZEL_TEST(codegen_choice_boxing_object_interface_3900,
           owning_choice_to_object_still_rejected_spec_8_6_35_8) {
    auto r = runAot(
        "module M3900D; "
        "class Res3900D public method Get(): int => 1; end Res3900D; "
        "choice Holder3900D Has(owned r: Res3900D), Empty3900D; "
        "end Holder3900D; "
        "class C "
        "  public static method Main(): int "
        "    var h: Holder3900D := Holder3900D.Has(new Res3900D()); "
        "    var o: Object := h; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot be boxed"));
}

// ── T5 — OWNERSHIP CONTROL (§35.8's borrow-bearing bullet): boxing a choice
// whose active case borrows a class instance must neither duplicate nor drop
// that borrow's ownership. Loop-amplified 100x; `is` is a non-escaping read
// (LlvmBackend_Setup.cpp's BoxEscapeWalker) so the box is cleanly released
// each iteration, isolating this row's ASan signal to the payload itself.
namespace {
const char* kBorrowBearingBoxLoop3900E =
    "module M3900E; "
    "class Res3900E public method Get(): int => 7; end Res3900E; "
    "choice Box3900E Filled(item: Res3900E), Empty3900E; end Box3900E; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..100 do "
    "      var r := new Res3900E(); "
    "      var b: Box3900E := Box3900E.Filled(r); "
    "      var o: Object := b; "
    "      if o is Box3900E then "
    "        total := total + r.Get(); "
    "      end; "
    "    end; "
    "    Console.WriteLine(total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

BEZEL_TEST(
    codegen_choice_boxing_object_interface_3900,
    borrow_bearing_choice_box_does_not_duplicate_or_drop_payload_ownership_spec_35_8) {
    JitResult plain = runAot(kBorrowBearingBoxLoop3900E);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("700\n"));
    JitResult r = runAotChecked(kBorrowBearingBoxLoop3900E);
    if (wasSkipped(r)) return;
    // A leak (box wrapper OR the borrowed Res3900E) or a double-free/UAF on
    // Res3900E's own scope-end disposal flips r.ran false under ASan/LSan.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── T6 — THE DECLINED-DEFAULT BRANCH (§35.7/§8.3): a BORROW-BEARING choice
// (never auto-hashable, §35.7 — "any borrow-bearing or owning choice" is
// excluded) is boxed and its `ToString`/`Equals`/`GetHashCode` are called
// through the box's Object-typed slots. None of T1-T5 exercise this branch —
// they all use `Shape3900*`, an auto-hashable choice. Per the commit's
// DECLINED-WITH-REASONS note: `ToString` falls back to the qualified type
// name (the §8.5 non-auto-hashable-body-record default, reused rather than
// invented); `Equals`/`GetHashCode` fall back to the ordinary §8.3 Object
// default (`boxDefaultEqualsThunk`/`boxDefaultHashCodeThunk` — reference/box
// identity), NOT a structural comparison the direct-call surface itself
// would reject as a compile error (§9.2's use-site rule, read through
// §35.7's choice extension of it).
BEZEL_TEST(codegen_choice_boxing_object_interface_3900,
           non_auto_hashable_choice_boxed_declined_defaults_spec_8_3_8_5_35_7) {
    auto r = runAot(
        "module M3900F; "
        "class Res3900F public method Get(): int => 1; end Res3900F; "
        "choice Box3900F Filled(item: Res3900F), Empty3900F; end Box3900F; "
        "class C "
        "  public static method Main(): int "
        "    var res := new Res3900F(); "
        "    var o1: Object := Box3900F.Filled(res); "
        "    var o2: Object := Box3900F.Filled(res); "
        "    Console.WriteLine(\"str \" + o1.ToString()); "
        "    Console.WriteLine(\"eqSelf \" + o1.Equals(o1).ToString()); "
        "    Console.WriteLine(\"eqDiffBox \" + o1.Equals(o2).ToString()); "
        "    Console.WriteLine(\"hashSelfStable \" "
        "      + (o1.GetHashCode() = o1.GetHashCode()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("str M3900F.Box3900F\n"
                                 "eqSelf True\n"
                                 "eqDiffBox False\n"
                                 "hashSelfStable True\n"));
}

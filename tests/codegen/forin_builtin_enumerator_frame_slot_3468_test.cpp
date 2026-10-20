// Regression tests for #3468 (spec §29.7 / §29.2, v0.286): the §29.7
// abandoned-iterator frame slot for a builtin `for-in`'s implicit enumerator.
//
// §29.7 requires an abandoned iterator's Dispose to run "the pending cleanup
// actions active at the point of suspension ... exactly as if the body had
// exited at the suspension point". For a `for var x in <builtin enumerable>`
// inside a yielding iterator, two such actions are IMPLICIT: the fresh
// enumerator the loop clones from the source (§29.11, vtable slot 4) and — when
// the source is loop-owned rather than a borrowed named binding — the source
// enumerable itself. Neither has a decl node, so #944 gives each a DEDICATED
// frame field, reserved by `collectSuspendLocals` via
// `forInBuiltinEnumeratorNeedsFrameSlot` and wired by
// `lowerIEnumerableBuiltinFor`.
//
// The two sites must agree. The predicate's own header comment said as much
// ("mirror lowerIEnumerableBuiltinFor's family check + the isArrayBacked ==
// false decision ... Any divergence would leave the wired-in enumerator slot
// pointing at a missing / mismatched field") — and the mirror had DRIFTED in
// four independent ways, each a real leak on abandonment because the lowering
// then falls back to the alloca spill area, which Dispose does not read:
//
//   1. `checkPureIEnumerable` accepted only `IEnumerable`, not
//      `IOwningEnumerable`      -> owning_local_source_*
//   2. it required an AST node, so every cross-module TypeRef answered false
//      (covered by the module-tier sibling; not reachable single-module)
//   3. no `FieldSymbol`/`PropertySymbol` bare-identifier arm (#1642)
//                                -> bare_identifier_property_source_*
//   4. no `MemberAccessExpr` (#1642) / `IndexExpr` (#3356) arms
//                                -> member_access_property_source_*,
//                                   indexer_source_*
//
// The fix does not close the four gaps; it DELETES the copy. Both sites now
// call `Emitter::classifyBuiltinForInSource`, so a fifth divergence is not a
// state the compiler can be in.
//
// A second, independent §29.7 gap is fixed alongside and pinned here: the yield
// snapshot dropped every `iterEnumDispose` disposal that was not one of the
// for-in's own frame-wired resources, on the (false) claim that such a disposal
// "cannot be live across this yield". A NAMED local bound to an iterator-format
// enumerable is exactly that: its scope-end disposal dispatches vtable slot 1,
// so it carries `iterEnumDispose`, and it was silently dropped ->
// named_local_source_*.
//
// ORACLE. Every leaked object here is a compiler-synthesised wrapper /
// enumerator frame, so no user destructor can count it: LeakSanitizer is the
// oracle (`runAotChecked`), and each program AMPLIFIES the abandonment to 200
// rounds so the leak cannot hide as a still-stack-reachable single allocation.
// Every case also asserts the computed value on stdout, so a program that
// stopped producing the right answer is not silently accepted as "clean".
//
// Spec authority: §29.7 *Disposal of partially-consumed iterators*, §29.2
// *Desugaring of `for`*, §29.11; stdlib §4.12–§4.13 (v0.286).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. Each row in this file used to guard on a
// local `wasSkipped` predicate whose whole body read the literal "SKIP" out
// of the result's diagnostics and returned from the row on it. That is an
// ABSORBING site by scripts/check_toolchain_skip_surface.py's own
// definition, and it is a silent vacuous pass: on a box whose clang cannot
// link the ASan binary, all 11 rows returned WITHOUT asserting and the suite
// reported a clean PASS having compiled and run nothing. MEASURED on the
// pre-change tree with the harness's own knob
// (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): 11 passed, 0 failed, and
// `CHECKED TOOLCHAIN: 11 skip(s)`.
//
// Each guard became the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r))`
// (tests/support/aot_result_asserts.hpp): the skip is now LOUD — the row
// FAILS — instead of being absorbed by a guard that reports a clean pass. That is strictly stronger than the guard it replaced — the row can no
// longer report green on a machine that never compiled the program — and
// every substantive assertion in every row (r.ran and the value pins) stays
// exactly where it was.

// One program shape, four knobs:
//   extraMembers - members added to `Src` (a property / an indexer)
//   bind         - statements run in `Walk` before the for-in (a local binding)
//   source       - the for-in source SPELLING under test
//   body         - the for-in body (the owning rows must discharge `x`)
//   abandon      - `break` out of the CONSUMER loop after the first element,
//                  suspending `Walk` INSIDE its own for-in (the §29.7 shape)
// `Run` is repeated 200 times so an abandonment leak accumulates well past the
// "still reachable from the stack" one-shot false negative.
std::string program(const std::string& extraMembers,
                    const std::string& bind,
                    const std::string& source,
                    const std::string& body,
                    bool abandon) {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Src\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3));\n"
        "    _a.Add(new Item(4));\n"
        "  end constructor;\n"
        + extraMembers +
        "  public method Walk(): IEnumerable<int>\n"
        + bind +
        "    for var x in " + source + " do\n"
        + body +
        "    end for;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 10 + v;\n"
        + (abandon ? std::string("      break;\n") : std::string()) +
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

const char* kBorrowBody = "      yield return x.V;\n";
// §29.2 / §28.3: an `IOwningEnumerable<T>` loop variable OWNS each element and
// the body must discharge it; `using var u := owned x;` is that discharge and
// is itself a §29.7 cleanup action, so the element is reclaimed on abandonment
// too. (Without a discharge the auto-drop never runs on the abandoned edge and
// the ELEMENT leaks — a separate defect, deliberately not conflated here.)
const char* kOwningBody =
    "      using var u := owned x;\n"
    "      yield return u.V;\n";

const char* kSeqProperty =
    "  public property Seq: IEnumerable<Item> => _a;\n";
const char* kSeqIndexer =
    "  public property [i: int]: IEnumerable<Item> => _a;\n";

// Abandoned: every round takes only the first element, so t = 200 * 3.
const char* kAbandonedOut = "t=600";
// Fully drained: every round folds 3 then 4, so t = 200 * 34.
const char* kDrainedOut = "t=6800";

void expectClean(const JitResult& r, const char* expectOut) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expectOut) != std::string::npos);
}

}  // namespace

// --- divergence 4a: MemberAccessExpr (`this.Seq`, a computed property read) --
// The mirror had no MemberAccessExpr arm at all, so no frame field was reserved
// while the lowering took the vtable path and wired one. BOTH implicit
// resources leaked: the GetEnumerator clone AND the wrapper frame the property
// read mints (the source is a COMPUTED property, so the loop owns it, #1840 /
// #2779). Measured on 907ce0f: 25600 B / 800 allocations over 400 rounds.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           member_access_property_source_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program(kSeqProperty, "", "this.Seq", kBorrowBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

// --- divergence 3: bare identifier resolving to a PropertySymbol (#1642) -----
// Same source, spelled without `this.`. The lowering's IdentifierExpr arm has a
// Field/Property branch; the mirror's did not, so this shape leaked identically
// (25600 B / 800 on 907ce0f) even though its `this.`-qualified twin above is
// the same read.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           bare_identifier_property_source_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program(kSeqProperty, "", "Seq", kBorrowBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

// --- divergence 4b: IndexExpr (#3356) ---------------------------------------
// An indexer whose declared return type is `IEnumerable<T>`: the read wraps the
// raw backing into a vtable-backed enumerator (#3356), so the loop is on the
// vtable path — but the mirror considered only CallExpr / NewExpr /
// IdentifierExpr. Measured 25600 B / 800 on 907ce0f.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           indexer_source_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program(kSeqIndexer, "", "this[0]", kBorrowBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

// --- divergence 1: the OWNING head in checkPureIEnumerable ------------------
// `lowerIEnumerableBuiltinFor` admits `IEnumerable` OR `IOwningEnumerable` for a
// local/param source (`isSyncIterFormatEnumerableHead`); the mirror hard-coded
// `== "IEnumerable"`, so the owning spelling reserved nothing and its
// GetOwningEnumerator clone leaked. Measured 9600 B / 400 on 907ce0f (plus the
// named local's own 9600 B / 400, the second gap below).
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           owning_enumerable_local_source_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program("", "    var seq: IOwningEnumerable<Item> := _a;\n", "seq",
                kOwningBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

// --- the SECOND, independent §29.7 gap: a named iterator-format local -------
// This shape is the one the mirror ALREADY handled, so its enumerator was never
// the leak. What leaked was the local `seq` itself: `lowerLocalDecl` mints a
// vtable-backed wrapper for an `IEnumerable<T>`-typed local (#1177) and
// registers a scope-end `iterEnumDispose`, and the yield snapshot dropped every
// `iterEnumDispose` that was not one of the for-in's own frame-wired slots.
// Measured 12800 B / 400 on 907ce0f, and UNCHANGED by spelling it `using var` —
// which is what proves it is not the plain-`var`/unwind-only rule.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           named_local_source_wrapper_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program("", "    var seq: IEnumerable<Item> := _a;\n", "seq",
                kBorrowBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           named_local_source_using_var_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(
        program("", "    using var seq: IEnumerable<Item> := _a;\n", "seq",
                kBorrowBody, /*abandon=*/true));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kAbandonedOut);
}

// --- controls: FULL DRAIN must not double-free ------------------------------
// Giving a for-in a frame field it never had changes what the iterator's
// Dispose runs. The abandoned edge and the exhausted edge are discriminated by
// the suspend state (Dispose stamps -2 before running the stack; exhaustion
// parks -1, which has no case in the switch), so the loop's own scope-exit
// disposal must remain the SOLE disposal when the consumer drains. A regression
// here is a double-free, which ASan reports exactly like the leak does.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           member_access_property_source_full_drain_disposes_once_29_7) {
    JitResult r = runAotChecked(
        program(kSeqProperty, "", "this.Seq", kBorrowBody, /*abandon=*/false));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kDrainedOut);
}

BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           indexer_source_full_drain_disposes_once_29_7) {
    JitResult r = runAotChecked(
        program(kSeqIndexer, "", "this[0]", kBorrowBody, /*abandon=*/false));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kDrainedOut);
}

BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           owning_enumerable_local_source_full_drain_disposes_once_29_7) {
    JitResult r = runAotChecked(
        program("", "    var seq: IOwningEnumerable<Item> := _a;\n", "seq",
                kOwningBody, /*abandon=*/false));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kDrainedOut);
}

BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           named_local_source_full_drain_disposes_once_29_7) {
    JitResult r = runAotChecked(
        program("", "    var seq: IEnumerable<Item> := _a;\n", "seq",
                kBorrowBody, /*abandon=*/false));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kDrainedOut);
}

// --- the ARRAY-protocol side of the same classifier -------------------------
// `classifyBuiltinForInSource` answers the family gate AND the protocol; a
// widening that pulled an array-backed source onto the vtable path would read
// `bz_array_t.data` as a vtable pointer (a SIGSEGV, #2815's shape). An
// `IReadOnlyList<T>` source keeps the indexer-based array protocol and creates
// no enumerator at all, so it must reserve no frame field and iterate correctly
// while suspended.
BEZEL_TEST(forin_builtin_enumerator_frame_slot_3468,
           read_only_list_source_keeps_array_protocol_29_2) {
    JitResult r = runAotChecked(
        program("  public property Ro: IReadOnlyList<Item> => _a;\n", "",
                "this.Ro", kBorrowBody, /*abandon=*/false));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, kDrainedOut);
}

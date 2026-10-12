// Codegen regression test for #5652: a WHOLE-VALUE MOVE out of an OWNING
// CHOICE **FIELD**, `var t := move h.C;`, spec v0.334.
//
// This is the FIELD twin of `codegen_choice_move_keyword_local_test.cpp`
// (which pins the same transfer out of a SOURCE LOCAL, `var t := move src;`).
// The two share machinery and were fixed separately; this file is the one that
// was still open when that sibling landed.
//
// §15.3 *Move-only value types* names two move-only kinds — the owning inline
// array and the owning-classified choice — and §9.4 *Value receivers are
// borrows* *Move-only receivers* draws the SAME conclusion for both:
//   "and so is the storage an **instance field** of either kind vacates under
//    a whole-value `move` (`var t := move h.C;` …), from the move until the
//    restoration store replaces the value".
// §15.3's *Local lifetime* bullet repeats it for the field source, and §28.8.1
// *The vacated location* generalizes the posture to every vacated field and
// slot. §35.4 *Field placement* is what admits the shape at all: "a move-only
// value carrying no borrows may be a field of a class; the class's destructor
// disposes … the active case's payloads".
//
// THE BUG. `lowerUnary`'s `move`-of-a-field arms (LlvmBackend_Exprs2.cpp)
// gated the vacating on
//     isOwnedReferenceField(fs) || isOwnedIteratorInterfaceField(fs) ||
//     ownedCollectionFieldType(fs) != nullptr ||
//     owningInlineArrayFieldType(fs) != nullptr
// There was NO owning-CHOICE predicate in that gate. All four are
// pointer-typed or `[N x ptr]`, and an owning choice field is stored INLINE as
// a `{ i32 tag, case payloads… }` struct (LlvmBackend_Layout.cpp:2495
// `choiceStruct`), so no arm was reached for it at all: the move read the
// struct out and wrote NOTHING back to the field's storage. The field kept the
// payload pointer, so the taker's scope-end active-case pass and the holder's
// §28.4 step-2 pass (LlvmBackend_Disposal.cpp:1593 →
// `emitOwningChoiceLocationDisposal`) BOTH dispatched `Dispose` through it.
//
// THE MARK IS THE ENCODING THE SOURCE LOCAL ALREADY USES. `lowerUnary`'s arms
// already zero out the field for the owning INLINE ARRAY
// (`nullMovedFieldStorage`), and the source LOCAL's mark is a whole-struct
// runtime zero-store (`markOwningChoiceMovedFromCore`,
// LlvmBackend_Disposal.cpp:7112). Its rationale rejects the alternative in
// terms, at LlvmBackend_Disposal.cpp:6938:
//     "WHY NOT A SENTINEL TAG instead of a zero-store: an out-of-range tag
//      would also make the disposal switch fall to its no-op default, but it
//      puts the slot in a state the CASE lowering's exhaustive switch has no
//      arm for. Zero is an in-range, constructible state."
// So the disposed-state ENCODING question this lane was asked to decide
// between a zero-store and a tombstone is ALREADY DECIDED for the owning
// choice, in the codebase, with a recorded rationale. What was missing is
// only the FIELD call site. This file pins the zero-store, not a new
// tombstone.
//
// WHY IT IS NOT A LEAK. Zeroing leaves the tag at 0 and every payload pointer
// null, and every consumer of the field's storage already treats that as
// holding nothing:
//   * the holder's §28.4 step-2 pass reaches the per-tag switch, whose DEFAULT
//     arm is the join block (LlvmBackend_Disposal.cpp:3472) — a case-0 payload
//     pointer is then null-guarded before dispatch
//     (LlvmBackend_Disposal.cpp:3604, "Null-guard: a never-bound arm … or a
//     payload-defaulted construction"), so it disposes NOTHING;
//   * the TAKER holds the only ownership and disposes it exactly once.
// So the payload is disposed exactly once, by the taker, and the mark is a
// VACANCY rather than a permanent disablement — which
// `spec_28_7_fresh_store_re_arms_the_vacated_field` pins from the other side.
// §28.7 *Moved-from and null*: "A moved-from or null location therefore stores
// without disposal."
//
// MEASURED at `r23-c28` (b403ebf8), ASan on the generated code: the two pinning
// rows below FAIL on that base and pass with the fix —
// `heap-use-after-free`, READ of size 8 in `bz$M.Holder$DisposeCore`, of a
// region freed by the taker and allocated in the field initializer. The
// pre-fix observable under `runAot` is a signal death, reported as
// `ran == false`. Loop-amplified by hand on the same base: the
// `spec_9_4_…disposes_exactly_once` program crashed in **300 of 300** one-shot
// runs and is **0 of 300** with the fix, 0 leaks either side.
//
// THE ORACLE, and why this file counts disposals rather than just exits. A
// double free can land in still-mapped memory, so a ONE-SHOT run is a vacuous
// oracle: the loop row counts disposals over 200 iterations, and a destructor
// that PRINTS makes a doubled disposal a duplicated LINE, distinguishable from
// a suppressed one (a leak) and from the spec's answer.
//
// SCOPE — WHAT THIS FILE DOES NOT PIN. The RESTORATION store
// `var t := move h.C; h.C := t;` is NOT pinned here. That shape still
// double-frees after this fix, through a DIFFERENT and already-identified
// site: `lowerOwnedChoiceFieldStore` (LlvmBackend_Setup.cpp:32103) runs the
// §28.7 drop-on-overwrite and then `CreateStore(newVal, fieldAddr)` without
// VACATING THE TAKER `t`, so both `t`'s scope-end pass and the field's still
// own the same payload. The mark installed here fixes the FIELD source; it
// cannot reach the taker, which is a LOCAL. That defect is separate, is not
// this commit's, and is recorded in the commit body. Consequently the
// conformance rows `s9_4_0010` and `s15_1_1_0009` (which both contain a
// restoration store) remain red and are NOT claimed by this file.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A destructor that PRINTS: one line per disposal, so a doubled disposal is a
// duplicated line rather than an indistinguishable crash.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Slot Full(owned r: Resource), Empty; end Slot; "
    "class Holder "
    "  public C: Slot; "
    "  public constructor() C := Slot.Full(new Resource(\"c\")); "
    "  end constructor; "
    "end Holder; ";

// A COUNTING destructor for the loop-amplified row: only an exact count over
// many iterations distinguishes "disposed twice" from "disposed once".
const char* kCountingResourceDecls =
    "class Resource "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource._n := Resource._n + 1; end destructor; "
    "end Resource; "
    "choice Slot Full(owned r: Resource), Empty; end Slot; "
    "class Holder "
    "  public C: Slot; "
    "  public constructor() C := Slot.Full(new Resource()); "
    "  end constructor; "
    "end Holder; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " class Host public static method Main() " + body +
                  " end Main; end Host;");
}

// Assert the program RAN to a clean exit and produced exactly `expected` on
// stdout. All three assertions are needed: a pre-fix double free is a signal
// death, reported as ran == false with a truncated stdout.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- §9.4 / §15.3: the field-source move vacates the field ------------------

BEZEL_TEST(codegen_choice_move_keyword_field,
           spec_9_4_move_keyword_out_of_an_owning_choice_field_vacates_it) {
    // THE PIN. §9.4: "the storage an **instance field** of either kind
    // vacates under a whole-value `move` … from the move until the restoration
    // store replaces the value: the value then lives in the taker".
    // Pre-fix: `moved` then SIGSEGV (and heap-use-after-free, READ of size 8,
    // in bz$M.Holder$DisposeCore, under --sanitize=address), because the field
    // kept the payload pointer `t` had taken and BOTH active-case passes
    // dispatched Dispose through it. Post-fix: `t` is the sole owner and the
    // payload is disposed exactly once.
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "var t := move h.C; "
            "Console.WriteLine(\"moved\");"),
        std::string("moved\ndispose c\n"));
}

// --- the LOOP row: an exact disposal count, not a one-shot exit -------------

BEZEL_TEST(codegen_choice_move_keyword_field,
           spec_9_4_move_out_of_an_owning_choice_field_disposes_exactly_once) {
    // 200 iterations, counting disposals. A ONE-SHOT double free can land in
    // still-mapped memory and pass on both sides of a fix; only an exact count
    // over many iterations distinguishes "disposed twice" from "disposed once".
    // MEASURED by hand on the same shapes with ASan on the generated code:
    // 300/300 crashes on the base at r23-c28 (b403ebf8), 0/300 with the fix,
    // 0 leaks on both sides. 200 is both the pre-fix value (the taker's and
    // the holder's passes both fire) and the correct post-fix value (the TAKER
    // is the sole owner, and every iteration builds a fresh pair, so nothing
    // else changes).
    expectRunOutput(
        run(kCountingResourceDecls,
            "for var i in 0..<200 do "
            "  using var h := new Holder(); "
            "  var t := move h.C; "
            "end for; "
            "Console.WriteLine($\"{Resource._n}\");"),
        std::string("200\n"));
}

// --- §28.7: a fresh store into the vacated source re-arms it ----------------

BEZEL_TEST(codegen_choice_move_keyword_field,
           spec_28_7_fresh_store_re_arms_the_vacated_field) {
    // The next store into a vacated field gives it a live value again, and BOTH
    // owners must then dispose exactly once each — two payloads, two lines, in
    // the order the spec requires (§28.4 step 2: the holder's pass runs at the
    // holder's own destruction). This is the row that pins the mark as a
    // VACANCY rather than a permanent disablement of the field's disposal: if
    // the fix had suppressed the field's pass outright, this row would print
    // only `dispose c` and lose `dispose c2`.
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "var t := move h.C; "
            "h.C := Slot.Full(new Resource(\"c2\")); "
            "Console.WriteLine(\"rearmed\");"),
        std::string("rearmed\ndispose c\ndispose c2\n"));
}

// --- a neighbour §28.8.1 declines: a MEMBER body on the field ---------------

BEZEL_TEST(codegen_choice_move_keyword_field,
           spec_28_8_1_a_read_of_the_field_does_not_vacate_it) {
    // §28.11's element-slot policy / §28.3's Scope: reading the field's CURRENT
    // value through a borrowing-match BINDING is a borrow, not a whole-value
    // transfer, so nothing is vacated and the payload stays live and is disposed
    // once. §19.3's *Pattern binding*: a `case` binding over a move-only
    // scrutinee "is a **borrow** of the value". This row is green on the BASE
    // and must STAY green — it is the guard against widening the mark to a
    // read. (Note the binding cannot be moved into a local: the compiler
    // rejects that at §15.3 *Local-to-local assignment*, so this row consumes
    // it only through the match itself.)
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "case h.C of "
            "  Full(_) => Console.WriteLine(\"borrowed\"), "
            "  Empty => Console.WriteLine(\"empty\") "
            "end case;"),
        std::string("borrowed\ndispose c\n"));
}
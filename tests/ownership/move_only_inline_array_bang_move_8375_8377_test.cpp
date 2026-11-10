// The null-forgiving read `a!` of a MOVE-ONLY owning inline-array local at a
// transfer position MOVES the local, exactly as the bare read `a` does
// (issues #8375, #8377; spec §17.8 / §15.3 / §28.3, v0.355).
//
// THE GOVERNING TEXT.
//
//   §17.8 (spec :3662) — "Over a named owned binding, `x!` is therefore
//   classified by its position exactly as the bare read `x` is". §17.8
//   (spec :3660): "The null-forgiving operator preserves ownership: if `x`
//   has ownership state *owned* ..., `x!` is owned ... it does not move, copy,
//   or borrow beyond what reading `x` already does."
//
//   §15.3 (spec :3038) — "A move-only value type is a value type whose
//   instances cannot be duplicated. Assignment transfers the value from source
//   to destination, leaving the source in moved-from state. Reading from a
//   moved-from location is a compile error." The two move-only kinds are the
//   owning inline array (`[N owned]T` / `[N owned]T?`) and the owning-classified
//   choice; the opening rule is kind-INDEPENDENT.
//
//   §15.3 (spec :3048), *Parameter passing* — "for a move-only V, it is by
//   value-move (transfer ownership). The caller's argument expression is in
//   moved-from state after the call."
//
//   §28.3 (spec :6666) — the consume table's producers: the moved-from read is
//   the "owned value in `x` was already moved out at this point" error.
//
// THE DEFECT. The ownership pass has ONE whole-move recognizer per move-only
// kind. The OWNING-CHOICE one (`noteOwningChoiceWholeMoveRead`) peels the `!`
// layers before its bare-identifier test (the #7725 fix); the OWNING-INLINE-
// ARRAY one (`noteMoveOnlyArrayWholeMoveRead`) required a bare
// `ast::IdentifierExpr` and so never fired on `a!`. The two kinds of §15.3
// therefore disagreed on the same spelling:
//
//   #8377 — `ASink.Take(a!);` did not mark `a` moved-from, so a later
//           `a[0].Tag` was ACCEPTED: a use-after-move the compiler did not
//           diagnose (the bare spelling `ASink.Take(a);` was already rejected).
//   #8375 — the SAME gap at the call-argument recording sites: an inline
//           array passed as `x!` to a move-only formal was not marked
//           moved-from, at all four of those sites.
//
// THE FIX is one mechanism in one function: peel the `!` layers, exactly as the
// owning-choice twin has since #7725. Nothing else changes — the bare
// spelling, the borrows, and the plain (copyable) array are untouched.
//
// THE POPULATION. The one primitive
// (`MethodOwnership::noteMoveOnlyArrayWholeMoveRead`) is the whole-move
// recognizer for BOTH issues, and it has exactly five call sites, all of them
// a transfer position:
//
//   1. `handleLocalDecl` (src/ownership/Ownership.cpp) — the local-decl
//      initializer, `var y := a!;`.
//   2. `recordMovesInArgs` (src/ownership/Ownership_Checks.cpp) — the
//      argument of a call whose RESULT IS BOUND, `var t := Take(a!);`.
//   3. `recordMovesIn`'s `new T(...)` arm — the argument of a CONSTRUCTOR
//      call whose result is bound, `var t := new Sink(a!);`.
//   4. the bare-STATEMENT call arm — `Take(a!);`.
//   5. the bare-STATEMENT constructor arm — `new Sink(a!);`.
//
// Sites 2-5 are the four call-argument recording sites #8375 names; site 1 is
// #8377's local-declaration twin. Each gets its own row below, so the closure
// covers the WHOLE population of both issues and not one probe of it.
//
// THE ORACLE. Each REJECT row pins the SPECIFIC moved-from diagnostic ("was
// already moved out"), so a row cannot pass on some unrelated error. Each
// ACCEPT row pins the RUNTIME OUTPUT (the exact disposal count) where one is
// available, so a row cannot pass by merely asserting "no diagnostic" — a
// doubled disposal (>100) and a suppressed one (<100, a leak) are both
// distinguishable from the spec's answer.
//
// NOT IN SCOPE. The move-only inline-array PARAMETER's missing scope-end
// discharge obligation (issue #8376) is a SEPARATE defect with a separate
// cause: the callee-side registration of a parameter's slot disposal lives in
// the code generator (`Emitter::registerUnwindOwnedParam`,
// src/codegen/LlvmBackend_Setup.cpp), not in the ownership pass, and that file
// is not this lane's to edit. This file therefore closes only #8375 and
// #8377, and pins no behaviour for #8376 either way.

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// The move-only inline-array program family. `Cell` is disposable-shaped (a
// destructor makes it non-trivial-dispose, so `[1 owned]Cell?` is a genuine
// move-only owning inline array) and its static `Count` counts the
// disposals, so a runtime row can pin the exact count.
//
// `body` is the body of `T`, which runs once per iteration.
std::string prog(const std::string& body) {
    return std::string(
               "module M; "
               "public class Cell "
               "  public static Count: int := 0; "
               "  public Tag: string; "
               "  public constructor(tag: string) Tag := tag; end constructor; "
               "  destructor Cell.Count := Cell.Count + 1; end destructor; "
               "end Cell; "
               "public class Sink "
               "  public static method Take(e: [1 owned]Cell?) end Take; "
               "  public constructor(e: [1 owned]Cell?) end constructor; "
               "end Sink; "
               "public class Program "
               "  public static method T() "
               "    var a: [1 owned]Cell? := [new Cell(\"x\")]; ") +
            body +
            "  end T; "
            "  public static method Main(): int "
            "    for var i in 0..<100 do T(); end; "
            "    Console.WriteLine($\"{Cell.Count}\"); "
            "    return 0; "
            "  end Main; "
            "end Program;";
}

// True iff some ERROR diagnostic reports the §28.3 moved-out use of `name`:
// the read/move form ("owned value in `name` was already moved out ...").
bool hasMovedOutError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("moved out") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// The ownership pass must REJECT the program with the moved-from diagnostic on
// `name`.
void expectMovedFromRead(const OwnershipResult& r, const std::string& name) {
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOutError(r, name));
}

// The exact disposal count of the runtime rows: one disposal per iteration,
// and no sanitizer finding (a double free or a leak would move the count or
// fail the run).
void expectExactlyOneDisposalPerIteration(const JitResult& r) {
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

}  // namespace

// =============================================================================
// #8377 — the whole-move recognizer must peel the `!`.
//
// The five rows below are the primitive's five call sites, one row each: the
// local-declaration twin (#8377's `m36`), the three call/ctor argument sites
// that bind their result (#8377's `m37` and the two constructor twins), and
// the bare-statement call site (#8377's `m05b`). Pre-fix: every one of them
// ACCEPTED.
// =============================================================================

// Site 1 — the local-declaration twin: §15.3 bullet 1 ("Local-to-local
// assignment `var y := x;` ... moves for a move-only value type") over the `!`
// spelling.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_localdecl_bang_initializer_is_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "var y := a!; "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// Site 2 — a call whose RESULT IS BOUND takes the `!`-spelled array as its
// argument: §15.3 *Parameter passing* ("The caller's argument expression is in
// moved-from state after the call").
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_bound_call_bang_argument_is_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "var t := Sink.Take(a!); "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// Site 3 — a CONSTRUCTOR call whose result is bound: the same §15.3
// *Parameter passing* bullet, at the constructor's own argument recording site.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_bound_ctor_bang_argument_is_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "var t := new Sink(a!); "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// Site 4 — the bare-STATEMENT call: the issue's own `m05b` repro. The call's
// result is discarded, so the argument is still the transfer.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_stmt_call_bang_argument_is_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "Sink.Take(a!); "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// Site 5 — the bare-STATEMENT constructor: the constructor twin of site 4.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_stmt_ctor_bang_argument_is_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "new Sink(a!); "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// =============================================================================
// #8375 — the SAME gap at the call-argument recording sites.
//
// #8375's OWN repro is the bare-statement call of site 4 (`Sink.TakeArr(a!);`
// then a read of `a`), which is why site 4 above IS #8375's repro row. This
// row is #8375's own repro verbatim, against the issue's own `Res`/`Sink`
// spelling and its own `[1 owned]Res?` element type — a DIFFERENT program
// from site 4's `Cell`/`Sink` family, so the two rows cannot pass or fail
// together by accident.
// =============================================================================

BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_parameter_passing_read_after_bang_argument_is_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Res "
        "  public Count: int := 0; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "public class Sink "
        "  public static method TakeArr(e: [1 owned]Res?) "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method Main(): int "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Sink.TakeArr(a!); "
        "    Console.WriteLine(a[0]!.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectMovedFromRead(r, "a");
}

// The PARAMETER SOURCE — the same §15.3 *Parameter passing* bullet, with a
// move-only inline-array PARAMETER as the argument instead of a local. §15.3
// makes a move-only parameter a by-value-move binding (the callee owns the
// value), so passing it onward with `e!` moves the parameter, and the later
// read of `e` is the same §15.3 compile error. Pre-fix: ACCEPTED (the `!`
// spelling was not recognized as a whole move, so the parameter stayed live).
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_bang_parameter_argument_is_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Cell "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "end Cell; "
        "public class Sink "
        "  public static method Take(e: [1 owned]Cell?) end Take; "
        "end Sink; "
        "public class Sink2 "
        "  public static method Take2(e: [1 owned]Cell?) "
        "    Sink.Take(e!); "
        "    Console.WriteLine(e[0]!.Tag); "
        "  end Take2; "
        "end Sink2; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [1 owned]Cell? := [new Cell(\"x\")]; "
        "    Sink2.Take2(a); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectMovedFromRead(r, "e");
}

// The PARAMETER SOURCE's BARE spelling — the control for the row above: the
// bare read of a move-only parameter at a transfer position already moved it,
// before the fix and after it. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_read_after_bare_parameter_argument_is_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Cell "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "end Cell; "
        "public class Sink "
        "  public static method Take(e: [1 owned]Cell?) end Take; "
        "end Sink; "
        "public class Sink2 "
        "  public static method Take2(e: [1 owned]Cell?) "
        "    Sink.Take(e); "
        "    Console.WriteLine(e[0]!.Tag); "
        "  end Take2; "
        "end Sink2; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [1 owned]Cell? := [new Cell(\"x\")]; "
        "    Sink2.Take2(a); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectMovedFromRead(r, "e");
}


// C3 — the BARE spelling of the same call: already rejected before the fix
// (the bare identifier satisfies the recognizer). Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_bare_call_argument_read_is_still_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "Sink.Take(a); "
        "Console.WriteLine(a[0]!.Tag); "));
    expectMovedFromRead(r, "a");
}

// C4 — an index borrow of the array is NOT a whole move, so it does not
// leave the array moved-from: the later whole read is fine. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_index_borrow_leaves_the_array_live) {
    auto r = parseAndCheckOwnership(prog(
        "Console.WriteLine(a[0]!.Tag); "
        "Console.WriteLine(a[0]!.Tag); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C4 — the `!`-spelled ELEMENT borrow `a![0]` is a borrow too, not a whole
// move: the array stays live and may be read again. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_bang_element_borrow_leaves_the_array_live) {
    auto r = parseAndCheckOwnership(prog(
        "Console.WriteLine(a![0]!.Tag); "
        "Console.WriteLine(a![0]!.Tag); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C4 — the `!`-spelled array as a `is` TEST scrutinee is a borrow, not a
// whole move: the array stays live. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_bang_is_test_leaves_the_array_live) {
    auto r = parseAndCheckOwnership(prog(
        "Console.WriteLine($\"{a! is [1 owned]Cell?}\"); "
        "Console.WriteLine(a[0]!.Tag); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C5 — a PLAIN (copyable) inline array `[1]int` is not a move-only value and
// must stay copyable: the call copies it and the source stays readable.
// Unchanged (the recognizer's move-only test still rejects it).
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_plain_inline_array_is_still_copyable) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Sink "
        "  public static method Take(a: [1]int) end Take; "
        "end Sink; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [1]int := [1]; "
        "    Sink.Take(a); "
        "    Console.WriteLine($\"{a[0]}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C6 — a move-only inline-array PARAMETER used as a BORROW (`e[0]` inside the
// callee) is a borrow of the callee's own binding, not a whole move of the
// caller's: the caller's array stays moved-from by the parameter passing rule
// alone, and the callee's borrow does not add a second error. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_move_only_parameter_borrow_is_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Cell "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; end constructor; "
        "end Cell; "
        "public class Sink "
        "  public static method Take(e: [1 owned]Cell?) "
        "    Console.WriteLine(e[0]!.Tag); "
        "  end Take; "
        "end Sink; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [1 owned]Cell := [new Cell(\"x\")]; "
        "    Sink.Take(a); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C7 — the `owned`-modified move-only array PARAMETER: the modifier is
// redundant per §15.3, and the peel changes nothing about it. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_owned_modified_move_only_parameter_is_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "public class Sink "
        "  public static method Take(owned e: [1 owned]Cell?) end Take; "
        "end Sink; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [1 owned]Cell? := [new Cell()]; "
        "    Sink.Take(a); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// C1 — the OWNING-CHOICE twin of the `!` spelling is UNCHANGED (it has peeled
// the `!` since #7725): the choice local is moved-from by the `!` spelling,
// and the bare spelling agrees with it.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_owning_choice_bang_spelling_is_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "public choice OC Text(owned r: Res), Empty; end OC; "
        "public class Program "
        "  public static method Main(): int "
        "    var b: OC? := OC.Text(new Res()); "
        "    var c := b!; "
        "    var n := b; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectMovedFromRead(r, "b");
}

// C2 — the OWNING-CHOICE twin's BARE spelling: the same moved-from verdict,
// unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_owning_choice_bare_spelling_is_unchanged) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "public choice OC Text(owned r: Res), Empty; end OC; "
        "public class Program "
        "  public static method Main(): int "
        "    var b: OC? := OC.Text(new Res()); "
        "    var c := b; "
        "    var n := b; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectMovedFromRead(r, "b");
}

// =============================================================================
// RUNTIME — the disposal COUNT, not merely "no diagnostic".
// =============================================================================

// The `!`-spelled local-to-local MOVE: the array is transferred to `y`, so
// exactly ONE disposal per iteration happens (by `y`). Unchanged on both arms
// (the codegen transfer walk already peeled the `!`), but pinned so a
// regression that both doubled and suppressed the disposal is caught.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_localdecl_bang_move_disposes_exactly_once) {
    expectExactlyOneDisposalPerIteration(
        runAotChecked(prog("var y := a!; ")));
}

// The `!`-spelled ELEMENT BORROW: `a` keeps the slots and disposes them at
// scope end — exactly ONE disposal per iteration. Unchanged.
BEZEL_TEST(ownership_move_only_inline_array_bang_8375_8377,
           spec_15_3_bang_element_borrow_disposes_exactly_once) {
    expectExactlyOneDisposalPerIteration(
        runAotChecked(prog("var s := a![0]!.Tag; ")));
}

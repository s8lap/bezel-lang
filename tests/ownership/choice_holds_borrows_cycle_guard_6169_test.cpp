// Ownership — #6169: `choiceHoldsBorrows`'s cycle guard was DEAD, so the
// §35.4 Owning walk recursed without bound on a mutually-recursive pair of
// generic choices and bezelc exhausted the stack (SIGSEGV, exit 139) instead
// of reaching §35.1's value-cycle rejection.
//
// The guard inserted `rt` into a `seen` set that was declared as a
// FUNCTION-LOCAL and never threaded into the nested-choice recurse, so every
// level built a fresh, empty set and the `insert` always succeeded. A pair
// like `choice A<T> X(b: B<T>)` / `choice B<T> Y(a: A<T>)` — both Owning via an
// `owned` leg — reached 52,317 `choiceHoldsBorrows` frames on a program
// declaring four choice types (measured). The `seen.erase(rt)` on exit was
// present and equally inert; the set never left the frame.
//
// The fix threads the set through the recursion with PATH semantics (insert on
// entry, erase on exit), mirroring `isBorrowBearingRecordImpl`'s #1611 guard —
// a TypeRef already on the CURRENT call chain contributes no new information,
// while a SIBLING payload may still re-classify the same TypeRef under its own
// substitution. Termination, NOT a depth cap: a cap that silently stopped
// checking would be this repo's top defect class.
//
// The trigger is the Owning leg, not a value's use: `classifyChoiceType`
// returning ValueLike/BorrowBearing short-circuits the walk before it recurses.
// Row (2) therefore drops the issue's extra locals and still crashed on base.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `Disp` is a heap class so the `owned` payload leg exists at all;
// `Inner<T>` marks its bare parameter `owned`; `A`/`B` are the mutually
// recursive pair. `Inner`/`Own` are kept because the issue's repro carries
// them, and row (1) is the issue's literal program.
const char* const kPrelude = R"(module M;
class Stats public static Drops: int := 0; end Stats;
class Disp
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Stats.Drops := Stats.Drops + 1; end destructor;
end Disp;
choice Own Held(owned r: Disp), Empty; end Own;
choice Inner<T> Has(owned v: T), No; end Inner;
choice A<T> X(b: B<T>), AN; end A;
choice B<T> Y(a: A<T>), Z(owned v: T), BN; end B;
class Program
)";

std::string program(const std::string& body) {
    return std::string(kPrelude) + "  public static method Main(): int\n" +
           body +
           R"(    return 0;
  end Main;
end Program;
)";
}

// §35.1's value-cycle diagnostic. Asserted by TEXT, not merely by "did not
// run": a compile crash and a rejection both leave `r.ran == false`, so the
// needle is what distinguishes the fixed behavior from the base SIGSEGV.
const char* const kValueCycle =
    "cannot recursively contain itself by value through its case payloads";

}  // namespace

// §35.1 / §35.4 — the issue's literal repro: the mutually recursive generic
// pair with an `owned` leg, with the value used. Expected: §35.1's
// value-cycle rejection, exit 1. Before the fix: bezelc SIGSEGV (exit 139).
BEZEL_TEST(choice_holds_borrows_cycle_guard_6169,
           mutually_recursive_generic_choices_rejected_spec_35_1) {
    JitResult r = runAot(program(
        "    var a := A<Disp>.AN;\n"
        "    var b := a;\n"
        "    var c := a;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kValueCycle));
}

// §35.4 / §35.1 — the SAME cycle with the issue's extra locals removed. The
// Owning leg, not a value's use, is what reaches the walk, so the crash is
// independent of the locals; before the fix this also SIGSEGV'd (exit 139),
// which the issue's own "once a value is used" framing understates.
BEZEL_TEST(choice_holds_borrows_cycle_guard_6169,
           mutually_recursive_generic_choices_rejected_without_any_use_spec_35_1) {
    JitResult r = runAot(program("    var a := A<Disp>.AN;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kValueCycle));
}

// §9.7 / §35.4 — NON-CYCLIC control, and the row that would go red if the
// path set were cut too eagerly. `Outer` is Owning and its payload
// `Inner`/`Loaded` is itself Owning with an UNMARKED `Log` class payload
// beside the `owned` one, so the walk recurses two levels and must still
// answer TRUE — the §35.4 `Loaded(owned response, log)` example. Nothing
// here is cyclic, so this row is unchanged by the fix in BOTH directions: it
// pins that threading `seen` did not turn the Owning walk into a blanket
// "no borrows" and start under-rejecting. Expected: the §9.7 class-field
// store rejection, exit 1.
BEZEL_TEST(choice_holds_borrows_cycle_guard_6169,
           acyclic_nested_owning_choice_still_detects_held_borrow_spec_35_4) {
    JitResult r = runAot(
        "module M; "
        "class Resp public N: int; public constructor(n: int) N := n; end constructor; end Resp; "
        "class Log public N: int; public constructor(n: int) N := n; end constructor; end Log; "
        "choice Loaded Owned(owned r: Resp, l: Log), Empty; end Loaded; "
        "choice Outer Wrapped(v: Loaded), None; end Outer; "
        "class Holder public F: Outer; public constructor(f: Outer) F := f; end constructor; end Holder; "
        "class Program public static method Main(): int return 0; end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow-bearing choice may not be stored in an instance field of a class"));
}

// §35.4 — the acyclic case that must still COMPILE AND RUN: an Owning choice
// nested one level inside another Owning choice, exercised 200 times. The
// disposal count is asserted as an explicit STDOUT oracle (`drops=200`)
// because the count is exact and does not depend on the sanitizer's
// availability — `runAot` builds no ASan/LSan net at all, so a row that wants
// the leak oracle has to ask for it (`runAotChecked`, which does pass
// `detectLeaks=true`; `runAotCheckedLenient` is the detect_leaks=0 one).
// Reading `drops` off stdout keeps this row armed identically on a build with
// no sanitizer and on one with it. A dropped disposal, or a walk that stopped
// early and let the move-only value through twice, moves this number.
BEZEL_TEST(choice_holds_borrows_cycle_guard_6169,
           acyclic_owning_choice_runs_and_disposes_once_spec_35_4) {
    JitResult r = runAot(
        "module M; "
        "class Stats public static Drops: int := 0; end Stats; "
        "class Disp public N: int; public constructor(n: int) N := n; end constructor; "
        "  destructor Stats.Drops := Stats.Drops + 1; end destructor; end Disp; "
        "choice Inner<T> Has(owned v: T), No; end Inner; "
        "choice Outer<T> Wrapped(v: T), None; end Outer; "
        "class Program "
        "  public static method Main(): int "
        "    for var k in 0..<200 do "
        "      using var o := Outer<Inner<Disp>>.Wrapped(Inner<Disp>.Has(new Disp(k))); "
        "    end; "
        "    Console.WriteLine(\"drops=\" + Stats.Drops.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("drops=200\n"));
}
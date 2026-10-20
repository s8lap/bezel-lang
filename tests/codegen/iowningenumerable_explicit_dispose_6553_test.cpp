// §28.3 / §29.1 / §29.2 / §28.11 / §28.2 (v0.338) — #6553: an EXPLICIT
// `d.Dispose()` on a callee-owned `IOwningEnumerable<T>` value disposed
// NOTHING, and suppressed the scope-bound auto-drop that would have.
//
// THE RULE. §28.3's consume table, row K3.9: `x.Dispose()` on an owned
// receiver is the DISCHARGE — post-state "explicitly-disposed", and the
// Disposal-moment column says "logical disposal at ... the call". The glossary
// entry for that post-state is what decides the obligation question this issue
// turns on: "explicitly-disposed — the binding after a site-announced kill that
// moves no obligation ... **Scope-end machinery skips its disposal**". So the
// EXPLICIT call owns the obligation and the auto-drop MUST stand down. The
// slot-null is that hand-off and it is exactly right; what was broken is that
// the call the hand-off was protecting had disposed nothing.
//
// THE DEFECT — and it is NEITHER of the two shapes the issue offered. Not a
// silent no-op (the call reached the wrong function and MUTATED the
// enumerator), and not a "disposed flag set without the disposal running" (the
// whole reclaim tail ran). `emitExplicitDisposeChain`'s user-class arm called
// `emitDisposeAndReclaim`, which dispatches vtable SLOT 2 — the CG35-itable
// CLASS convention (§8.5/§8.6 #417: [0]=td$, [1]=itmap$, [2]=Dispose). A value
// at an `IOwningEnumerable<T>` head is a compiler-minted frame in §29.2's
// ITERATOR format — [0]=self, [1]=Dispose, [2]=MoveNext, [3]=TakeCurrent,
// [4]=GetOwningEnumerator — so slot 2 is `MoveNext`. MEASURED with
// `BEZEL_DUMP_IR` on the base `4aaa4f21`: the call lowers to
// `%v = load ptr, ptr %d ; %s = getelementptr ptr, ptr %v, i32 2 ; call void
// %f(ptr %d)` against `@"bz$ownarrenum$ptr$own$vtable"`. So it called
// `MoveNext` — which drained ONE element into the frame's `pending` slot and
// discarded the i1 result — and the reclaim tail then `free`d the frame with
// that element still named by it.
//
// THE LEAK ARITHMETIC PINS THAT MECHANISM. ASan on the issue's repro reports
// 232 B in 5 allocations, and 232 = 56 (array header) + 128 (array data) +
// 3x16 (the Items): `Direct 16` is the ONE Item `MoveNext` popped into
// `pending` and which died with the freed frame, `Direct 56` is the array
// header, `Indirect 128` the array data, `Indirect 16 + 16` the two Items still
// in it. All five are accounted for by "slot 2 was MoveNext" and by no other
// shape.
//
// THE FIX. The discriminator #6372 already built for the for-in loop's
// loop-end disposal (`iterEnumOrClassDispose`) and #6381 unified into ONE
// definition: §29.2's positive shape test — every iterator vtable this
// compiler emits puts ITSELF in slot 0, every CLASS vtable a type descriptor —
// so `vtable[0] == vtable` is true for the iterator format and false for every
// class instance including a subclass. Asked of the VALUE, not of the static
// type, which is what makes it correct for both families.
//
// ORACLE. `runAotChecked` — generated-code ASan/LSan in process, so a leak OR a
// use-after-free OR a double-free turns `ran` false — plus the EXACT destructor
// count on stdout. The exact count is the ASan-grade oracle here: a double
// dispose (the hazard this fix could have introduced, had it dispatched slot 1
// WITHOUT standing the auto-drop down) prints a HIGHER number and fails, where
// a leak ceiling would pass at 0 as readily as at 32. A `did-not-run` sentinel
// keeps a leak distinguishable from a wrong count.
//
// The count is read from `Main` AFTER `Inner` returns, so what is asserted is
// the count at the disposal site itself, not one displaced by later frames.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT harness
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A counting destructor and a factory returning a callee-OWNED
// `OwningArray<Item>` at an `IOwningEnumerable<Item>` head — the §28.3 K3
// transfer that leaves the frame the collection's sole owner. `body` is what
// `Inner` does with the bound value.
std::string program(const std::string& body, int iters) {
    std::string adds;
    for (int i = 0; i < iters; ++i) adds += "c.Add(move (new Item())); ";
    return
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new OwningArray<Item>(); " + adds +
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int " + body +
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The shape the issue reports: a plain `var` owned local (so the scope-bound
// auto-drop IS registered and DOES exist) carrying an EXPLICIT `.Dispose()`.
// Both sites are live, and §28.3's "explicitly-disposed" glossary entry says
// the explicit one wins and the auto-drop stands down.
std::string binding(const std::string& body) {
    return "var d := C.Build(); " + body + " return 0;";
}

const char* kDidNotRun = "<did-not-run>";

std::string checkedStdout(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::cout << "    [6553] program did not run cleanly (compile reject, "
                     "leak, or sanitizer abort)\n";
        return kDidNotRun;
    }
    return r.stdoutText;
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.3 K3.9 / §29.2 — THE REPORTED SHAPE. An explicit `d.Dispose()` on a
// bound, never-iterated `IOwningEnumerable<Item>` must dispose every element
// (§28.11's row) and release the storage. Before the fix this printed 0 and
// leaked 232 B in 5 allocations (MEASURED on `4aaa4f21`), because slot 2 was
// `MoveNext`.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           explicit_dispose_on_bound_never_iterated_disposes_all_28_3_29_2) {
    std::string src = program(binding("d.Dispose();"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §29.2 / stdlib §4.13 — the SAME obligation after a PARTIAL read. The loop
// discharges what it took (§29.2 makes the loop variable an owned local
// discharged on every body-exit edge); the explicit `Dispose()` must then pay
// exactly the remainder, no more and no less. This row is the double-dispose
// oracle for the partial shape: 3 is correct, 4 would mean the explicit call
// re-disposed an element the loop had already taken, and 1 would mean it
// disposed only its own (wrong) view of the remainder.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           explicit_dispose_after_partial_read_pays_the_remainder_28_11) {
    std::string src = program(
        binding("for var e in d do using var u := e; break; end;"
                "d.Dispose();"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — the FULLY-ITERATED case: the drain emptied the collection, so the
// explicit Dispose frees storage only. 3, not 6 — and ASan would flag the
// double too. This is the row that would catch an over-eager fix which
// dispatched slot 1 while LEAVING the scope-bound auto-drop in place (the
// double-dispose direction the issue explicitly warns about): the auto-drop's
// null-guard reads the slot the explicit call nulled, so it skips, and the
// total stays 3.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           explicit_dispose_after_full_read_disposes_nothing_twice_28_11) {
    std::string src = program(
        binding("for var e in d do using var u := e; end;"
                "d.Dispose();"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.2 — a REPEATED explicit `d.Dispose()`. The first call discharges the
// binding; the second must be a clean no-op (§28.2: "the first call performs
// the disposal; subsequent calls are no-ops"), reading 3 and not 6. On a value
// whose slot the first call nulled, the second call's receiver reload is null
// and the chain's null-guard skips — so this also pins that the fix's
// unconditional slot-null in the iterator arm is what makes the repeat safe.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           repeated_explicit_dispose_is_a_noop_28_2) {
    std::string src = program(binding("d.Dispose(); d.Dispose();"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — amplified. A one-shot leak repro is a vacuous oracle (it can hide
// under the sanitizer's noise), so this loops 200x x 3 = 600 and asserts the
// EXACT total. 600 truncates to 8 bits through the AOT exit path (600 mod 256
// = 88), so the count goes on stdout and never in a return code. The exact
// total is also the exactly-once oracle across the explicit/auto-drop pair:
// the pre-fix code printed 0 with 600x the leak, a half-fix that ran the
// disposal but left the auto-drop live would abort under ASan, and a fix that
// ran both would print > 600.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           explicit_dispose_amplified_200x_no_leak_28_11) {
    std::string src =
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new OwningArray<Item>(); "
        "    c.Add(move (new Item())); c.Add(move (new Item())); "
        "    c.Add(move (new Item())); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    var k := 0; "
        "    while k < 200 do "
        "      var d := C.Build(); "
        "      d.Dispose(); "
        "      k := k + 1; "
        "    end while; "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "600\n");
}

// ---------------------------------------------------------------------------
// §28.3 K3.9 — the scope-bound DISCHARGE (`using var`), the spelling this
// issue's `d.Dispose()` is the explicit counterpart of. Already correct before
// the fix, and it must STAY correct — but be clear about what it is: a
// REGRESSION GUARD for the NEIGHBOURING path, not a control on this fix's
// decision. MEASURED: its IR never reaches the changed code at all (the
// auto-drop discharges the binding directly, with no `disp.iter`/`disp.cls`
// dispatch), so it is green on base and on the fix alike and it discriminates
// nothing about the discriminator. What it does protect is the neighbour that
// matters most: that the two discharge spellings — the explicit call this fix
// repairs and the scope-bound one it must not disturb — still both pay once.
// 3, once.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           scope_bound_using_var_discharge_still_disposes_once_28_10) {
    std::string src = program("using var d := C.Build(); return 0;", 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.3 cl.3 / §28.11 — the guard on the OTHER family, and the row that
// corrected this lane's OWN reasoning rather than the compiler's.
//
// I first wrote this row expecting 0, on the theory that a BORROWING
// `IEnumerable<T>` head must cascade nothing (§28.6) because the callee still
// owns its `OwningArray`. MEASUREMENT REFUTED THAT, and the spec says why:
// §28.3 clause 3 ("Containers: calls mint, storage lends") makes an
// iterable-family return type — `IEnumerable<T>` is one of the eight heads —
// MINT-ANNOUNCING, and adds that "callee-side, the unmarked mint-announcing
// return type transfers the container like an `owned` return". So the callee's
// local is GONE at return and the frame is the collection's sole owner, exactly
// as at the owning head. `BEZEL_DUMP_IR` confirms the two ends of it: Build's
// body ends `ret ptr %arrenumown` with NO `bz_array_free` (the callee pays
// nothing), and the frame it returns is `@"bz$arrenumown$ptr$Dispose"`, which
// DOES call `bz_array_free_owning_wrap(arr, null, /*consultHeader=*/1)` and so
// cascades. A `did-not-run`-clean run prints 3.
//
// SCOPE-SENSITIVE, and this row asserts the shape it actually is: binding the
// value inside a separate method (the shape here, `C.Inner`) prints 3, because
// `Build`'s frame has been returned by the time `Main` reads the count.
// Binding at `Main` scope instead prints 0 — the count is read before the
// discharge runs. MEASURED by the non-author review; the row deliberately
// binds inside the method.
//
// The value is the same on BOTH legs (MEASURED: base `4aaa4f21` and this fix
// both print 3, LSan-clean), which is what makes this a GUARD rather than a
// fail-without row.
//
// WHAT THIS ROW DOES AND DOES NOT HOLD DOWN. It does NOT hold down the slot-1
// guard. MEASURED BY MUTANT: under a mutant that hard-wires the discriminator
// to `true` (always slot 1), this row still PASSES — both enumerable heads mint
// the same §29.2 frame, so an over-eager fix that dropped the discriminator
// would slip past here. Only the LAST row catches that, because a class
// instance carries a real type descriptor in slot 0 and therefore takes the
// other arm. What this row does hold down is the opposite direction: a fix
// that over-reaches and disposes something twice. It is reported here as a
// control, and the FIVE rows above are the fail-without set.
//
// It also shows the runtime discriminator is the load-bearing choice. Both
// enumerable heads mint frames in the SAME §29.2 iterator format ([0]=self,
// [1]=Dispose, [2]=MoveNext) and differ only in what the frame's Dispose
// cascades — so a static "is this an enumerable head" gate and a value gate
// agree HERE, and the value gate is the one that also covers the class family
// (the last row), which the static reading would silently exclude.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           explicit_dispose_on_borrowing_head_cascades_the_transferred_28_3) {
    std::string src =
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IEnumerable<Item> "
        "    var c := new OwningArray<Item>(); "
        "    c.Add(move (new Item())); c.Add(move (new Item())); "
        "    c.Add(move (new Item())); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    var d := C.Build(); "
        "    d.Dispose(); "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.2 / §28.12 — THE CLASS-FAMILY GUARD, and the row that says the fix's
// runtime discriminator did not simply redirect every explicit Dispose to
// slot 1. A plain user class is a CLASS vtable ([0]=vt$C, [2]=Dispose), so
// `vtable[0] == vtable` is false and the CLASS arm must run unchanged: the
// destructor runs AND the 16-byte block is reclaimed. Before the fix this row
// was already green, and a fix that dropped the discriminator and hard-wired
// slot 1 would call a garbage function pointer and crash here.
//
// This is the ONE load-bearing control for the discriminator, and MEASURED BY
// MUTANT to be so: under a mutant that hard-wires the discriminator to `true`
// (always slot 1) this is the ONLY row of the eight that fails. The
// enumerable-family rows cannot catch it — both heads mint the same §29.2
// frame, so they would all still pass. Keep this row last and keep it.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_explicit_dispose_6553,
           class_family_explicit_dispose_still_uses_slot_two_28_2) {
    std::string src =
        "module M; import System.Console; "
        "class Res "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Res; "
        "class C "
        "  public static method Inner(): int "
        "    var r := new Res(); "
        "    r.Dispose(); "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Res._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "1\n");
}
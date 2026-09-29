// Codegen — move-only owning inline-array slot disposal (§15.3 / §15.1.1, #747).
//
// Spec §15.3 line 2021: "a move-only local's owned slots are auto-disposed at
// scope end by the compiler." §15.1.1 line 1957: "When an owning inline array's
// lifetime ends, the array's auto-dispose pass disposes each non-null slot."
// Before this fix, a `[N owned]T?` / `[N owned]T` local registered NO disposal
// at all, so its slots' class instances leaked on every exit (normal and
// unwind).
//
// SOUNDNESS: a move-only array whose ownership is TRANSFERRED out of the local
// (whole-array move `var b := a`, owned-arg, `return arr`, …) must NOT be
// disposed at the source's scope end (the receiver / callee owns it) — else a
// double-free. The fix gates disposal on a conservative default-transfer
// pre-pass and nulls a slot at `move a[i]`. All "disposed" assertions use a
// static counter that a double-free would drive negative or corrupt.

// AOT MIGRATION (#1769 / #1763): this file runs under the AOT driver (runAot).
// Every disposal / soundness observable here is ≤255 and round-trips the exit
// code — AOT reproduces the JIT results exactly, EXCEPT the aliasing-store case
// `a[i] := a[j]`: the JIT test harness skips the ownership pass and fell back to
// a conservative "safe leak" (the codegen marks the array ineligible), but the
// REAL driver (AOT) runs the §28.11 ownership check and REJECTS the store — the
// precise fix the pre-migration comment flagged as a follow-up is in fact present
// in the front end. That test now asserts the AOT rejection (see below). This is
// AOT being stricter-and-correct, not a codegen bug (#1769).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff the front end rejected `src` (a diagnostic was reported and the
// program did not run) AND at least one message contains `needle`.
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Cell: a disposable class with a static live-instance counter. ctor +1,
// destructor -1. After all owners are disposed the counter is 0; a leak leaves
// it positive; a double-free drives it negative (or aborts).
const char* kCell =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

std::string prog(const std::string& body) {
    return std::string(kCell) +
        "class C public static method Main(): int " + body + " end Main; end C;";
}
}  // namespace

// §15.1.1 line 1957: an owning inline-array FIELD is disposed by the enclosing
// class's destructor. A Holder with a `[2 owned]Cell?` field → both slots
// disposed when the Holder is disposed (counter 0; was 2, a leak).
BEZEL_TEST(codegen_inline_owned_747, field_disposed_by_host_destructor) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "class Holder public slots: [2 owned]Cell? := "
        "  [new Cell(1), new Cell(2)]; end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(); end; return Cell.n; "
        "end Main; end C;"), 0);
}

// The non-nullable owning inline-array field likewise.
BEZEL_TEST(codegen_inline_owned_747, non_nullable_field_disposed_by_host) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "class Holder public slots: [2 owned]Cell := "
        "  [new Cell(1), new Cell(2)]; end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(); end; return Cell.n; "
        "end Main; end C;"), 0);
}

// A field slot moved out (`move this.slots[0]`) is nulled, so the host
// destructor skips it — the moved value is disposed once by its new owner and
// the remaining slot once by the host. No double-free.
BEZEL_TEST(codegen_inline_owned_747, field_slot_move_then_null_no_double_free) {
    BEZEL_EXPECT_EQ(run(std::string(kCell) +
        "class Holder public slots: [2 owned]Cell? := "
        "  [new Cell(1), new Cell(2)]; "
        "  public method DropFirst() "
        "    using var t: Cell? := move slots[0]; slots[0] := null; "
        "  end DropFirst; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(); h.DropFirst(); end; return Cell.n; "
        "end Main; end C;"), 0);
}

// §15.3: a `[N owned]Cell?` literal-initialized local disposes BOTH slots at
// scope end → counter back to 0.
BEZEL_TEST(codegen_inline_owned_747, nullable_literal_disposed_at_scope_end) {
    BEZEL_EXPECT_EQ(run(prog(
        "do var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; end; "
        "return Cell.n;")), 0);
}

// §15.3: the NON-nullable `[N owned]Cell` form likewise disposes its slots.
BEZEL_TEST(codegen_inline_owned_747, non_nullable_literal_disposed_at_scope_end) {
    BEZEL_EXPECT_EQ(run(prog(
        "do var a: [2 owned]Cell := [new Cell(1), new Cell(2)]; end; "
        "return Cell.n;")), 0);
}

// SOUNDNESS — whole-array move-only transfer `var b := a`: b's slots ALIAS a's
// pointers (aggregate copy). a is transferred → its disposal is SKIPPED; b (the
// receiver) disposes each cell exactly once → counter 0 (NOT -2 from a
// double-free, NOT a crash).
BEZEL_TEST(codegen_inline_owned_747, whole_array_move_no_double_free) {
    BEZEL_EXPECT_EQ(run(prog(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var b: [2 owned]Cell? := a; "
        "end; "
        "return Cell.n;")), 0);
}

// SOUNDNESS — `move a[i]` nulls the slot, so the scope-end disposal skips it and
// the moved value is disposed exactly once by its new owner. Slot 0 → `t`; slot
// 1 → the scope-end pass. Counter 0.
BEZEL_TEST(codegen_inline_owned_747, slot_move_then_null_restore_no_double_free) {
    BEZEL_EXPECT_EQ(run(prog(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  using var t: Cell? := move a[0]; "
        "  a[0] := null; "
        "end; "
        "return Cell.n;")), 0);
}

// BORROW position — an index read keeps the array disposal-eligible: the value
// is read (borrow), then both slots are disposed at scope end. Returns the read
// value (5) with counter 0 (encoded n*100+read).
BEZEL_TEST(codegen_inline_owned_747, index_read_borrow_stays_eligible) {
    BEZEL_EXPECT_EQ(run(prog(
        "var read: int := 0; "
        "do "
        "  var a: [2 owned]Cell? := [new Cell(5), new Cell(7)]; "
        "  read := a[0]!.V; "
        "end; "
        "return Cell.n * 100 + read;")), 5);
}

// BORROW position — a for-in iterable keeps the array eligible: iterate (borrow
// each slot), accumulate, then dispose both slots. sum=12, counter 0.
BEZEL_TEST(codegen_inline_owned_747, for_in_borrow_stays_eligible) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sum: int := 0; "
        "do "
        "  var a: [2 owned]Cell? := [new Cell(5), new Cell(7)]; "
        "  for var c in a do sum := sum + c!.V; end; "
        "end; "
        "return Cell.n * 100 + sum;")), 12);
}

// SOUNDNESS (adversarial verify, LENS 2) — a slot store `a[i] := a[j]` would
// alias one object into two owned slots; disposing both would double-free / UAF.
// The REAL driver (AOT) runs the §28.11 ownership check and REJECTS the store at
// compile time: "a borrow cannot be stored into an owning array slot". (The JIT
// test harness skips the ownership pass and instead fell back to a conservative
// codegen "safe leak" — marking the array ineligible — which is why the
// pre-migration runJit assertion observed a leak of 2 rather than this rejection.
// AOT is stricter-and-correct here; #1769.)
BEZEL_TEST(codegen_inline_owned_747, slot_alias_assignment_rejected_28_11) {
    BEZEL_EXPECT_TRUE(rejectedWith(prog(
        "var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "a[0] := a[1]; "        // borrow stored into an owning slot → §28.11 reject
        "return Cell.n;"),
        "owning array slot"));
}

// §15.3 / §27.5 (#782, was #747 LENS 1) — a move-only array captured by a
// NON-escaping closure is capture-boxed (its [N x ptr] value moves into a heap
// cell). The plain [N x ptr]-alloca disposal is skipped (the boxed slot is now a
// ptr-holder), but the elements are now disposed THROUGH the cell at scope end
// (after the closure release), so both cells are reclaimed: counter 0 (was a leak
// of 2). The capture still works (got == 1). No crash, no double-free.
BEZEL_TEST(codegen_inline_owned_747, closure_capture_disposes_via_cell_782) {
    BEZEL_EXPECT_EQ(run(prog(
        "var got: int := 0; "
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var f := lambda() => a[0]!.V; "  // captures a (capture-boxed)
        "  got := f(); "
        "end; "
        "return Cell.n;")), 0);  // got==1 read OK; both cells disposed via cell
}

// #782: TWO non-escaping closures capture the same array — they share ONE boxed
// cell, so the element-dispose-through-cell fires exactly once. Both cells
// reclaimed (counter 0); a double-free would drive the live counter negative.
BEZEL_TEST(codegen_inline_owned_747, captured_array_multi_closure_disposes_once_782) {
    BEZEL_EXPECT_EQ(run(prog(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  var f := lambda() => a[0]!.V; var g := lambda() => a[1]!.V; "
        "  var s := f() + g(); "
        "end; "
        "return Cell.n;")), 0);  // both disposed exactly once
}

// #782: a null slot is skipped — only the non-null element is disposed. One Cell
// created, one disposed → counter 0.
BEZEL_TEST(codegen_inline_owned_747, captured_array_null_slot_skipped_782) {
    BEZEL_EXPECT_EQ(run(prog(
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), null]; "
        "  var f := lambda() => a.Length; var n := f(); "
        "end; "
        "return Cell.n;")), 0);  // the one Cell disposed; null slot a no-op
}

// #782 (soundness boundary): an ESCAPING closure captures the array (stored in an
// outer var that outlives the do-block). The array is NOT in cellFreeEligible_,
// so it is NOT disposed at scope end (the escaped closure still references it) —
// a conservative leak (counter 2), NEVER a use-after-free / double-free (which
// would drive the live counter negative). The escaped closure reads valid memory.
BEZEL_TEST(codegen_inline_owned_747, captured_array_escaping_not_disposed_782) {
    BEZEL_EXPECT_EQ(run(prog(
        "var captured: () -> int := lambda() => 0; "
        "do "
        "  var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "  captured := lambda() => a.Length; "
        "end; "
        "var n := captured(); "
        "return Cell.n;")), 2);  // leaked (escaping), no double-free
}

// #806 (fixed): the array is declared inside a LOOP body and captured by a
// non-escaping closure. A field-by-field (per-slot GEP) initialiser means
// boxLocalSlot's whole-slot freshness heuristic misses it, so it would use
// GUARDED boxing (one cell reused across iterations) — which previously had to
// exclude the in-loop case (reusing then re-disposing the same cell would
// UAF/double-free), leaving the elements to leak (counter 4 = 2 iters x 2
// cells). #806 forces FRESH boxing for an in-loop inline-owned-array capture, so
// each iteration owns its own cell and the #782 element-dispose-through-cell
// fires per iteration. All 4 cells are now disposed (counter 0); a double-free
// across iterations would drive the live counter negative.
BEZEL_TEST(codegen_inline_owned_747, captured_array_in_loop_disposed_806) {
    BEZEL_EXPECT_EQ(run(prog(
        "for var i in 1 .. 2 do "
        "  do "
        "    var a: [2 owned]Cell? := [new Cell(1), new Cell(2)]; "
        "    var f := lambda() => a.Length; var n := f(); "
        "  end; "
        "end; "
        "return Cell.n;")), 0);  // #806: per-iteration fresh cell → all disposed
}

// #806: an in-loop captured inline-owned-array whose elements are READ through
// the closure each iteration. The fresh per-iteration cell holds that
// iteration's elements; reading a[0] returns the live value, and both cells are
// disposed at the iteration's scope end (counter 0 across both iterations).
BEZEL_TEST(codegen_inline_owned_747, captured_array_in_loop_read_disposed_806) {
    BEZEL_EXPECT_EQ(run(prog(
        "var acc: int := 0; "
        "for var i in 1 .. 3 do "
        "  do "
        "    var a: [2 owned]Cell? := [new Cell(i), new Cell(i)]; "
        "    var f := lambda() => a[0]!.V; acc := acc + f(); "
        "  end; "
        "end; "
        "return Cell.n;")), 0);  // each iteration's pair disposed; nothing leaks
}

// EXCEPTION path (§15.3 line 2022 / §28.3 implicit try/finally): both slots are
// disposed when an exception unwinds out of the array's scope. Mirrors the
// exception_dispose_test.cpp convention exactly (resource in a `do…end` inside
// the try, an UP-counting destructor, a case-arm catch reading the count) so the
// unwind-ordering is the established one — `Dn` increments once per slot
// disposed, so the catch sees 2.
BEZEL_TEST(codegen_inline_owned_747, exception_unwind_disposes_both_slots) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class AppError: Exception end AppError; "
        "class Dn "
        "  public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Dn.n := Dn.n + 1; end destructor; "
        "end Dn; "
        "class C public static method Main(): int "
        "  try "
        "    do var a: [2 owned]Dn? := [new Dn(), new Dn()]; "
        "       throw new AppError(); end; "
        "  catch AppError => return Dn.n; end; "
        "  return -1; "
        "end Main; end C;"), 2);
}

// fix #4552 (§15.1.1 v0.307, ruling #4388 on top of #747's shape): "the
// array's auto-dispose pass disposes each non-null slot, in reverse slot
// order — the same order ... as the whole-value-reassignment pass of §15.3
// ... one pass definition governs all four of this paragraph's endings"
// (scope end, instance-field Dispose(), whole-value reassignment, static
// program exit). Before this fix, the scope-end and instance-field-Dispose
// passes both iterated 0..N-1 (establishment order) while the reassignment
// pass (#3263/#1385) already iterated N..1 — this file's own #747 leak tests
// use a plain counter and cannot see the ORDER bug, only whether disposal
// happened at all. These tests assert order via each slot's own printed
// name, mirroring rule-corpus cases s15_1_1_0005/0006/0007
// (tests/conformance/rules/cases).
namespace {
// Named: like Cell, but prints its own name on disposal instead of
// incrementing a counter, so the *order* of disposal is observable on stdout.
const char* kNamed =
    "module M; "
    "class Named "
    "  public Name: string; "
    "  public constructor(name: string) Name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {Name}\"); end destructor; "
    "end Named; ";

std::string runOut(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// §15.1.1 v0.307: a 2-slot owning inline-array LOCAL disposes in reverse slot
// order at scope end — "second" (slot 1) before "first" (slot 0).
BEZEL_TEST(codegen_inline_owned_747, local_scope_end_disposes_reverse_slot_order) {
    BEZEL_EXPECT_EQ(runOut(std::string(kNamed) +
        "class C public static method Main() "
        "  do var a: [2 owned]Named := "
        "    [new Named(\"first\"), new Named(\"second\")]; "
        "     Console.WriteLine(a[0].Name); end; "
        "  Console.WriteLine(\"after\"); "
        "end Main; end C;"),
        "first\ndispose second\ndispose first\nafter\n");
}

// §15.1.1 v0.307: a 3-slot variant, so the assertion pins a genuine reversal
// (N..1) rather than a 2-element sequence a swap-based bug could pass by
// accident.
BEZEL_TEST(codegen_inline_owned_747, local_scope_end_disposes_reverse_slot_order_n3) {
    BEZEL_EXPECT_EQ(runOut(std::string(kNamed) +
        "class C public static method Main() "
        "  do var a: [3 owned]Named := "
        "    [new Named(\"a\"), new Named(\"b\"), new Named(\"c\")]; "
        "  end; "
        "end Main; end C;"),
        "dispose c\ndispose b\ndispose a\n");
}

// §15.1.1 v0.307: an owning-inline-array INSTANCE FIELD's disposal (the
// enclosing class's compiler-generated Dispose()) is reverse slot order too —
// "one pass definition governs all four ... endings".
BEZEL_TEST(codegen_inline_owned_747, instance_field_dispose_reverse_slot_order) {
    BEZEL_EXPECT_EQ(runOut(std::string(kNamed) +
        "class Pool "
        "  private _conns: [2 owned]Named; "
        "  public constructor() "
        "    _conns := [new Named(\"first\"), new Named(\"second\")]; "
        "  end constructor; "
        "  public method FirstName(): string => _conns[0].Name; "
        "end Pool; "
        "class C public static method Main() "
        "  do using var p := new Pool(); "
        "     Console.WriteLine(p.FirstName()); end; "
        "  Console.WriteLine(\"after\"); "
        "end Main; end C;"),
        "first\ndispose second\ndispose first\nafter\n");
}

// §15.1.1 v0.307 + §28.7: whole-value reassignment already disposed the
// DISPLACED value in reverse order (#3263) — this pins that the NEW value's
// eventual scope-end disposal is ALSO reverse order (the bug this fix closes),
// composed with the reassignment's own reverse pass in one program, matching
// rule-corpus case s15_1_1_0007.
BEZEL_TEST(codegen_inline_owned_747, reassign_then_scope_end_both_reverse_slot_order) {
    BEZEL_EXPECT_EQ(runOut(std::string(kNamed) +
        "class C public static method Main() "
        "  var pool: [2 owned]Named := [new Named(\"a1\"), new Named(\"a2\")]; "
        "  pool := [new Named(\"b1\"), new Named(\"b2\")]; "
        "  Console.WriteLine(\"stored\"); "
        "end Main; end C;"),
        "dispose a2\ndispose a1\nstored\ndispose b2\ndispose b1\n");
}

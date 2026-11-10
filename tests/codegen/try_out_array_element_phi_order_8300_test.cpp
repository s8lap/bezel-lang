// Codegen — an `out` ARRAY-ELEMENT recipient for the extraction-`Try` family
// must not emit malformed IR (#8300).
//
// WHAT WAS BROKEN.  The extraction-`Try` lowerings in the codegen built their
// merge block like this:
//
//     builder_.CreateBr(endBB);
//     builder_.SetInsertPoint(endBB);
//     emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);   // the copy-OUT
//     auto* phi = builder_.CreatePHI(i1(), 2, "trypop.r");     // the result phi
//
// i.e. the §22.3 copy-OUT half of the accessor bracket was emitted BEFORE the
// result phi.  For an `out` target that needs no bracket (`out x`, `out _`) the
// copy-OUT emits nothing, so the phi stayed first in the block and the IR
// verified.  For an ACCESSOR-MEDIATED target (`out a[0]`) the copy-OUT emits
// the slot's value load and the collection setter's store — and the setter's
// own bounds check BRANCHES, so the copy-OUT splits the merge block.  The phi
// was then created in the block the split left behind, no longer at the top of
// its block, and LLVM's verifier rejected the module:
//
//     PHI nodes not grouped at top of basic block!
//       %trypop.r = phi i1 [ true, %trypop.found ], [ false, %entry ]
//     label %chk.cont
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// SPEC AUTHORITY.  stdlib §4 (Stack/Queue): `public method TryPeek(out item:
// T?): bool;` and its `TryPop` / `TryDequeue` siblings; spec §22.3: an `out`
// argument may be ANY assignable location — `a[0]` is a legal `out` target, and
// the call must compile, assign `a[0]` on the found path and null on the miss
// path.  §22.6: the `out _` discard shape.  §28.3 / §28.9: ownership of the
// `out` value.
//
// THE FIX.  The result phi is created FIRST — at the top of the merge block,
// whose predecessors are exactly the phi's two incoming edges — and the §22.3
// copy-OUT flush runs after it.  That is the ordering the already-working
// `Dictionary.TryGetValue(k, out a[0])` twin uses (there the flush is the last
// thing emitted in the merge block and there is no phi after it).
//
// WHAT THESE ROWS PIN.  Every row below is a §22.3 accessor-mediated `out`
// recipient on one of the six sites that carried the defect, so before the fix
// each of them FAILED TO COMPILE ("IR verification failed"); the rows assert
// the program's stdout, so a regression back to malformed IR shows up as the
// "<did-not-run>" sentinel rather than as a silent pass.  The last three rows
// are the CONTROLS — the two shapes that already worked (`out x` local, and
// `Dictionary.TryGetValue`) plus the `out _` discard of §22.6 — and they pin
// that the fix did not disturb them.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A module with `body` as the statements of `Main`.
std::string main_(const std::string& body) {
    return std::string("module M; import System; class C "
                       "public static method Main(): int ") +
           body + " return 0; end Main; end C;";
}

// Assert the program's stdout byte-for-byte, and that it exited 0.
void expectOut(const std::string& body, const std::string& want) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// A class element, for the owning families (§4.10 / §4.11 / §4.12 / §4.7 all
// admit class elements only).
const char* kBox =
    "class Box public n: int := 0; "
    "  public constructor(x: int) n := x; end constructor; "
    "end Box; ";

std::string withBox(const std::string& body) {
    return std::string("module M; import System; ") + kBox + "class C "
           "public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

}  // namespace

// ── stdlib §4: Stack<T>.TryPeek / TryPop, Queue<T>.TryPeek / TryDequeue ────

// The issue's own repro, reduced to its observable: the call compiles, the
// found path assigns the extracted string into `a[0]`, and `a[0]` READS BACK
// as that string.  Before the fix this program did not compile at all.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           stack_trypeek_out_array_element_assigns_value) {
    expectOut(
        "var c := new Stack<string>(); c.Push(\"x\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "if c.TryPeek(out a[0]) then Console.WriteLine(\"hit:\" + (a[0] ?? \"<null>\")); "
        "else Console.WriteLine(\"miss:\" + (a[0] ?? \"<null>\")); end; ",
        "hit:x\n");
}

// The MISS path assigns NULL into the array element (spec §22.3 / stdlib §4
// "the false path assigns null"), overwriting the element's prior value —
// so the read-back is "<null>", not the pre-existing "init".
BEZEL_TEST(try_out_array_element_phi_order_8300,
           stack_trypeek_out_array_element_miss_assigns_null) {
    expectOut(
        "var c := new Stack<string>(); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "if c.TryPeek(out a[0]) then Console.WriteLine(\"hit:\" + (a[0] ?? \"<null>\")); "
        "else Console.WriteLine(\"miss:\" + (a[0] ?? \"<null>\")); end; ",
        "miss:<null>\n");
}

// The returned bool is unchanged by the fix: true on the found path, false on
// the miss path.  Both calls share one recipient array element, so the second
// call's null assignment is visible too.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           stack_trypeek_out_array_element_return_value_semantics) {
    expectOut(
        "var c := new Stack<string>(); c.Push(\"x\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(c.TryPeek(out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var d := new Stack<string>(); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(d.TryPeek(out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\nx\nFalse\n<null>\n");
}

// Stack.TryPop — the same site as TryPeek (one arm lowers both).
BEZEL_TEST(try_out_array_element_phi_order_8300,
           stack_trypop_out_array_element_assigns_value) {
    expectOut(
        "var c := new Stack<string>(); c.Push(\"x\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(c.TryPop(out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var d := new Stack<string>(); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(d.TryPop(out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\nx\nFalse\n<null>\n");
}

// Queue.TryPeek — the FRONT of the queue is the element written out.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           queue_trypeek_out_array_element_assigns_value) {
    expectOut(
        "var q := new Queue<string>(); q.Enqueue(\"y\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(q.TryPeek(out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var r := new Queue<string>(); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(r.TryPeek(out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\ny\nFalse\n<null>\n");
}

// Queue.TryDequeue.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           queue_trydequeue_out_array_element_assigns_value) {
    expectOut(
        "var q := new Queue<string>(); q.Enqueue(\"y\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(q.TryDequeue(out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var r := new Queue<string>(); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(r.TryDequeue(out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\ny\nFalse\n<null>\n");
}

// §4.11 with a NULLABLE element type (`Stack<string?>`) — the nullable-element
// lowering stores a tagged slot rather than a bare pointer, so it is a distinct
// path from the `string` rows above.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           stack_trypeek_out_array_element_nullable_element_type) {
    expectOut(
        "var c := new Stack<string?>(); c.Push(\"x\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(c.TryPeek(out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var d := new Stack<string?>(); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(d.TryPeek(out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\nx\nFalse\n<null>\n");
}

// ── the OWNING flavour (stdlib §4.11 / §4.12, the OwningStack / OwningQueue
//    arms of the same lowering) ────────────────────────────────────────────────

BEZEL_TEST(try_out_array_element_phi_order_8300,
           owning_stack_trypeek_out_array_element_assigns_value) {
    auto r = runAot(withBox(
        "var s := new OwningStack<Box>(); s.Push(new Box(5)); "
        "var a := new Array<Box?>(); a.Add(null); "
        "Console.WriteLine(s.TryPeek(out a[0])); "
        "Console.WriteLine((a[0]?.n) ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n5\n");
}

BEZEL_TEST(try_out_array_element_phi_order_8300,
           owning_queue_trydequeue_out_array_element_assigns_value) {
    auto r = runAot(withBox(
        "var q := new OwningQueue<Box>(); q.Enqueue(new Box(6)); "
        "var a := new Array<Box?>(); a.Add(null); "
        "Console.WriteLine(q.TryDequeue(out a[0])); "
        "Console.WriteLine((a[0]?.n) ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n6\n");
}

// ── OwningArray.TryPopFirst / TryPopLast (stdlib §4.5) ──────────────────────

BEZEL_TEST(try_out_array_element_phi_order_8300,
           owning_array_trypopfirst_out_array_element_assigns_value) {
    auto r = runAot(withBox(
        "var o := new OwningArray<Box>(); o.Add(new Box(7)); "
        "var a := new Array<Box?>(); a.Add(null); "
        "Console.WriteLine(o.TryPopFirst(out a[0])); "
        "Console.WriteLine((a[0]?.n) ?? -1); "
        "var b := new Array<Box?>(); b.Add(null); "
        "Console.WriteLine(o.TryPopLast(out b[0])); "
        "Console.WriteLine((b[0]?.n) ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n7\nFalse\n-1\n");
}

// ── OwningLinkedList.TryPopFirst / TryPopLast (stdlib §4.7) ─────────────────

BEZEL_TEST(try_out_array_element_phi_order_8300,
           owning_linkedlist_trypopfirst_out_array_element_assigns_value) {
    auto r = runAot(withBox(
        "var l := new OwningLinkedList<Box>(); l.AddLast(new Box(8)); "
        "var a := new Array<Box?>(); a.Add(null); "
        "Console.WriteLine(l.TryPopFirst(out a[0])); "
        "Console.WriteLine((a[0]?.n) ?? -1); "
        "var b := new Array<Box?>(); b.Add(null); "
        "Console.WriteLine(l.TryPopLast(out b[0])); "
        "Console.WriteLine((b[0]?.n) ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n8\nFalse\n-1\n");
}

// ── OwningSet.TryPop (stdlib §4.10) ──────────────────────────────────────────

BEZEL_TEST(try_out_array_element_phi_order_8300,
           owning_set_trypop_out_array_element_assigns_value) {
    auto r = runAot(
        "module M; import System; "
        "class Box public n: int := 0; "
        "  public constructor(x: int) n := x; end constructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is Box then var b := other as Box; return b!.n = n; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => n; "
        "end Box; "
        "class C public static method Main(): int "
        "var s := new OwningSet<Box>(); s.TryAdd(new Box(5)); "
        "var q := new Box(5); "
        "var a := new Array<Box?>(); a.Add(null); "
        "Console.WriteLine(s.TryPop(q, out a[0])); "
        "Console.WriteLine((a[0]?.n) ?? -1); "
        "var b := new Array<Box?>(); b.Add(null); "
        "Console.WriteLine(s.TryPop(q, out b[0])); "
        "Console.WriteLine((b[0]?.n) ?? -1); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n5\nFalse\n-1\n");
}

// ── the byte-buffer span flavour (stdlib §4.15, TryRead*) ───────────────────

BEZEL_TEST(try_out_array_element_phi_order_8300,
           span_tryread_out_array_element_assigns_value) {
    auto r = runAot(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "using var b := new Array<byte>(4, 0); b[0] := byte(1); "
        "var a := new Array<int>(); a.Add(-1); "
        "Console.WriteLine(b.AsReadOnlySpan().TryReadInt32LittleEndian(out a[0])); "
        "Console.WriteLine(a[0]); "
        "using var s := new Array<byte>(2, 0); "
        "var c := new Array<int>(); c.Add(-2); "
        "Console.WriteLine(s.AsReadOnlySpan().TryReadInt32LittleEndian(out c[0])); "
        "Console.WriteLine(c[0]); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n1\nFalse\n0\n");
}

// ── CONTROLS — the shapes that already worked MUST keep working ─────────────

// The `out` LOCAL form (§22.3's plain assignable location).  No bracket, no
// copy-OUT, so the copy-OUT flush emits nothing and the phi was always first
// in its block: the fix must leave this shape's behaviour untouched.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           out_local_form_is_unchanged) {
    expectOut(
        "var c := new Stack<string>(); c.Push(\"x\"); "
        "var x: string? := null; "
        "Console.WriteLine(c.TryPeek(out x)); "
        "Console.WriteLine(x ?? \"<null>\"); "
        "var d := new Stack<string>(); "
        "var y: string? := \"init\"; "
        "Console.WriteLine(d.TryPeek(out y)); "
        "Console.WriteLine(y ?? \"<null>\"); ",
        "True\nx\nFalse\n<null>\n");
}

// `Dictionary.TryGetValue(k, out a[0])` — THE WORKING TWIN (stdlib §4.8).  Its
// merge block never had a phi after the copy-OUT, so it was always correct;
// the fix must not change it.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           dictionary_trygetvalue_out_array_element_is_unchanged) {
    expectOut(
        "var d := new Dictionary<string, string>(); d.Add(\"k\", \"x\"); "
        "var a := new Array<string?>(); a.Add(\"init\"); "
        "Console.WriteLine(d.TryGetValue(\"k\", out a[0])); "
        "Console.WriteLine(a[0] ?? \"<null>\"); "
        "var b := new Array<string?>(); b.Add(\"init\"); "
        "Console.WriteLine(d.TryGetValue(\"zz\", out b[0])); "
        "Console.WriteLine(b[0] ?? \"<null>\"); ",
        "True\nx\nFalse\n<null>\n");
}

// The `out _` DISCARD (spec §22.6, §28.3) — the discarded `out owned` value
// is an unbound owned temporary that is disposed at the end of the full
// statement.  The fix must not disturb that path.
BEZEL_TEST(try_out_array_element_phi_order_8300,
           out_discard_form_is_unchanged) {
    expectOut(
        "var c := new Stack<string>(); c.Push(\"x\"); "
        "Console.WriteLine(c.TryPeek(out _)); "
        "Console.WriteLine(c.TryPop(out _)); "
        "Console.WriteLine(c.TryPop(out _)); "
        "var q := new Queue<string>(); q.Enqueue(\"y\"); "
        "Console.WriteLine(q.TryPeek(out _)); "
        "Console.WriteLine(q.TryDequeue(out _)); "
        "Console.WriteLine(q.TryDequeue(out _)); ",
        "True\nTrue\nFalse\nTrue\nTrue\nFalse\n");
}

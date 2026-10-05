// Regression tests for #5305 — `Channel<Record>` Send/TryReceive emitted
// malformed IR ("Invalid bitcast  %13 = bitcast %record.P %12 to i64" followed
// by "Trunc only produces integer  %tr.coerce = trunc i64 %tr.i64 to
// %record.P"), aborting `bezelc` at IR verification.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   Spec §30.14 (v0.313), value-type bullet: "All primitives and enums are
//   sendable; a **record** is sendable iff it is not borrow-bearing and every
//   field's type is sendable, an **inline array** iff its element type is
//   sendable, and a **choice** per §35.12 — each kind's own components ... A
//   sendable value crosses as a copy."
//   Spec §30.15 (v0.313): "A **channel** carries sendable values from one
//   domain to another ... their element type must be sendable."
//   Stdlib §6.9: `public class Channel<T> where T: sendable`; for sendable
//   value-typed T, "`Send(item: T)`, `TryReceive(out item: T?): bool`,
//   `Receive(): Task<T>`".
//
// So `Channel<record P(X: int, Y: int)>` is well-formed, and the item must
// round-trip with its field values intact.
//
// THE DEFECT. The channel carries ONE ptr-sized word per item. The Send
// lowering encoded a scalar by `zext`/`bitcast`-ing it into that word; a
// record struct has no such ≤64-bit encoding, and the `bitcast %record.P to
// i64` it fell into is not a legal LLVM instruction. The TryReceive decode had
// the matching hole: its "fallback for unexpected inner types (e.g. record
// struct)" stored the raw i64 and then asked `CreateIntCast` to coerce an i64
// to the record struct — "Trunc only produces integer".
//
// THE FIX. An AGGREGATE element (record §30.14 / choice §35.12, keyed by
// `sema::isAggregateChannelElement`) crosses as a POINTER to a fresh heap cell
// holding a copy of the value — §30.14's "A sendable value crosses as a copy"
// — encoded by `encodeChannelItemWord` and decoded by
// `decodeChannelItemWord`. The cell is released by the decoder, and by the
// channel's `dispose_elem` for an item still buffered when the channel is
// disposed or a send is refused (§30.15's undelivered-items rule). All three
// Send sites and both decode sites share the one codec.
//
// NOT COVERED HERE: nothing is rejected. `ReceiveAll()` on an
// aggregate-element channel is normative (§30.15: the drain yields "each item
// with `Receive`'s transfer semantics"; stdlib §6.9 gives `ReceiveAll():
// IAsyncEnumerable<T>` for sendable value-typed T) and is fixed here too —
// the drain reads the same item word and unboxes it at the loop variable.
//
// ROWS. User rows (record/choice specific — each FAILS pre-fix): the issue's
// own repro; a pre-declared recipient; `out var`; `out _`; 200-iteration
// round-trip; a record with a `string` field; a choice element; the
// `SendAsync` + `TryReceive` shape; the `await Receive()` shape; the
// `ReceiveAll()` drain, its `break` edge, and its suspend/resume edge; the
// undrained channel (LSan: the buffered cells are released at disposal); a send
// refused because the channel is full and one refused because it is closed
// (both dispose the cell). Control rows (the ordinary element kinds, green in
// EVERY leg including the pre-fix leg): int, long, double, bool, string, enum
// and a sealed class element.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// `record P(X: int, Y: int);` — the issue's element type. All-value fields, so
// §30.14 admits it as sendable and it is a legal `Channel<T>` element.
const char* kP = "record P(X: int, Y: int); ";

// Compile + run, asserting the exact stdout and a clean exit. The exact-stdout
// oracle is what makes a "silently wrong value" decode fail: printing the
// received fields catches a cell ADDRESS read as the record's bytes.
void expect(const std::string& src, const std::string& want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The same, under generated-code ASan + LSan: the oracle for the CELL lifetime
// (an unreleased cell is a leak; a double-released one is a double-free).
void expectClean(const std::string& src, const std::string& want) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;   // ASan toolchain unavailable — infra skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ---- the issue's own repro -------------------------------------------------
// Pre-fix: `bezelc` aborts with "Invalid bitcast ... Trunc only produces
// integer" (r.ran false, no executable).

BEZEL_TEST(channel_record_element_5305, issue_repro_send_tryreceive) {
    expect("module M; " + std::string(kP) +
           "class C public static method Main(): int "
           "  using var cr := new Channel<P>(); "
           "  using var tr := cr.NewSender(); "
           "  tr.Send(new P(3, 4)); "
           "  var o: P? := null; "
           "  if cr.TryReceive(out o) then Console.WriteLine(o!.X + o!.Y); end; "
           "  return 0; end Main; end C;",
           "7\n");
}

// ---- recipient shapes (§22.3 / §22.4 / §17.13) ----------------------------

BEZEL_TEST(channel_record_element_5305, predeclared_recipient) {
    expect("module M; " + std::string(kP) +
           "class C public static method Main(): int "
           "  using var cr := new Channel<P>(); "
           "  using var tr := cr.NewSender(); "
           "  tr.Send(new P(10, 20)); "
           "  var o: P? := null; "
           "  cr.TryReceive(out o); "
           "  Console.WriteLine($\"{o!.X},{o!.Y}\"); "
           "  return 0; end Main; end C;",
           "10,20\n");
}

BEZEL_TEST(channel_record_element_5305, out_var_recipient) {
    expect("module M; " + std::string(kP) +
           "class C public static method Main(): int "
           "  using var cr := new Channel<P>(); "
           "  using var tr := cr.NewSender(); "
           "  tr.Send(new P(1, 2)); "
           "  if cr.TryReceive(out var v) then Console.WriteLine(v!.X * v!.Y); "
           "  else Console.WriteLine(\"none\"); end; "
           "  return 0; end Main; end C;",
           "2\n");
}

// The `out _` discard: the received cell must still be released even though no
// binding survives (§17.13 / §28.3). Pre-fix: the malformed IR abort.
BEZEL_TEST(channel_record_element_5305, out_discard_releases_cell) {
    expectClean("module M; " + std::string(kP) +
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 200 do "
                "    tr.Send(new P(i, i)); "
                "    cr.TryReceive(out _); "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(cr.Count()); "
                "  return 0; end Main; end C;",
                "0\n");
}

// ---- repeated round-trip (the one-shot-oracle guard) -----------------------
// 200 send/receive pairs with the accumulated field sum printed: a decode that
// returned the cell address or a stale value fails here, not on a lucky first
// iteration.

BEZEL_TEST(channel_record_element_5305, repeated_round_trip_200) {
    expectClean("module M; " + std::string(kP) +
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; var acc := 0; var n := 0; "
                "  while i < 200 do "
                "    tr.Send(new P(i, i * 2)); "
                "    var o: P? := null; "
                "    if cr.TryReceive(out o) then "
                "      acc := acc + o!.X + o!.Y; n := n + 1; "
                "    end; "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(acc); Console.WriteLine(n); "
                "  Console.WriteLine(cr.TryReceive(out _)); "
                "  return 0; end Main; end C;",
                "59700\n200\nFalse\n");
}

// ---- a record with a `string` field (§30.14: `string` is sendable) ---------
// The cell holds a byte-copy of the record, so the row also pins that the
// shared-reference `string` survives the copy (an imbalance would show as an
// LSan leak or a use-after-free over 200 iterations).

BEZEL_TEST(channel_record_element_5305, record_with_string_field) {
    expectClean("module M; record P(S: string, N: int); "
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; var acc := 0; "
                "  while i < 200 do "
                "    tr.Send(new P(\"payload-value\", i)); "
                "    var o: P? := null; "
                "    if cr.TryReceive(out o) then "
                "      if o!.S = \"payload-value\" then acc := acc + o!.N; end; "
                "    end; "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(acc); "
                "  return 0; end Main; end C;",
                "19900\n");
}

// ---- a choice element (§35.12) --------------------------------------------

BEZEL_TEST(channel_record_element_5305, choice_element) {
    expectClean("module M; choice Sh A(i: int), B(s: string); end Sh; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<Sh>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(Sh.B(\"hi\")); "
                "  tr.Send(Sh.A(9)); "
                "  var a: Sh? := null; var b: Sh? := null; "
                "  if cr.TryReceive(out a) then "
                "  if cr.TryReceive(out b) then "
                "    case a! of "
                "      A(var n) => do Console.WriteLine(n); end, "
                "      B(var t) => do Console.WriteLine(t); end "
                "    end; "
                "    case b! of "
                "      A(var n) => do Console.WriteLine(n); end, "
                "      B(var t) => do Console.WriteLine(t); end "
                "    end; "
                "  end; end; "
                "  return 0; end Main; end C;",
                "hi\n9\n");
}

// ---- the async shapes: SendAsync and Receive -------------------------------
// `SendAsync` hands the SAME item word to the same runtime entry as `Send`, and
// `await ch.Receive()` reads it back out of the task's result buffer — the two
// other sites of this defect.

BEZEL_TEST(channel_record_element_5305, send_async_then_try_receive) {
    expectClean("module M; import System.Threading; " + std::string(kP) +
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  await tr.SendAsync(new P(3, 4)); "
                "  var o: P? := null; "
                "  if cr.TryReceive(out o) then Console.WriteLine(o!.X + o!.Y); end; "
                "  return 0; end Main; end C;",
                "7\n");
}

BEZEL_TEST(channel_record_element_5305, receive_awaits_record) {
    expectClean("module M; import System.Threading; " + std::string(kP) +
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(new P(3, 4)); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o.X + o.Y); "
                "  return 0; end Main; end C;",
                "7\n");
}

// ---- the undelivered-cell rows (§30.15) ------------------------------------
// A cell still buffered when the channel is disposed, when a send is refused
// because the channel is FULL, and when one is refused because it is CLOSED —
// each must be released rather than leaked. Pre-fix these rows are unreachable
// (the Send lowering aborts the compiler), so they are user rows too.

BEZEL_TEST(channel_record_element_5305, undrained_cells_released_at_disposal) {
    expectClean("module M; " + std::string(kP) +
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 50 do tr.Send(new P(i, i)); i := i + 1; end; "
                "  Console.WriteLine(cr.Count()); "
                "  return 0; end Main; end C;",
                "50\n");
}

BEZEL_TEST(channel_record_element_5305, full_channel_refused_send_releases_cell) {
    expectClean("module M; " + std::string(kP) +
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(1); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(new P(1, 2)); "
                "  try "
                "    tr.Send(new P(3, 4)); "
                "    Console.WriteLine(\"no-exc\"); "
                "  catch e: ChannelFullException => "
                "    Console.WriteLine(\"full\"); "
                "  end; "
                "  var o: P? := null; "
                "  if cr.TryReceive(out o) then Console.WriteLine($\"{o!.X},{o!.Y}\"); end; "
                "  return 0; end Main; end C;",
                "full\n1,2\n");
}

BEZEL_TEST(channel_record_element_5305, closed_channel_refused_send_releases_cell) {
    expectClean("module M; " + std::string(kP) +
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  do using var t1 := cr.NewSender(); end; "
                "  using var t2 := cr.NewSender(); "
                "  try "
                "    t2.Send(new P(1, 2)); "
                "    Console.WriteLine(\"no-exc\"); "
                "  catch e: ChannelClosedException => "
                "    Console.WriteLine(\"closed\"); "
                "  end; "
                "  return 0; end Main; end C;",
                "closed\n");
}

// ---- the drain form (§30.15 / stdlib §6.9) ---------------------------------
// `ReceiveAll()` is normative for sendable value-typed T: §30.15 requires the
// drain to yield "each item with `Receive`'s transfer semantics", and stdlib
// §6.9 gives it for value T as `ReceiveAll(): IAsyncEnumerable<T>`. The
// drain's element handoff is async-enumerable vtable slot 3, which returns the
// channel's item WORD — the same word `Receive` produces — so the loop
// variable needs the same unbox (channelDrainHasAggregateElement). MEASURED
// without it: the drain ran the right number of iterations and printed the
// CELL ADDRESS as the record's fields (`913422560,223003` for `3,4`).

BEZEL_TEST(channel_record_element_5305, receive_all_drains_record_elements) {
    expectClean("module M; import System.Threading; " + std::string(kP) +
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<P>(); "
                "  var tr := cr.NewSender(); "
                "  tr.Send(new P(3, 4)); "
                "  tr.Send(new P(7, 8)); "
                "  do using var t := move tr; end; "     // drop sender => ch closes
                "  var n := 0; "
                "  await for var v in cr.ReceiveAll() do "
                "    Console.WriteLine($\"{v.X},{v.Y}\"); n := n + 1; "
                "  end; "
                "  Console.WriteLine(n); "
                "  return 0; end Main; end C;",
                "3,4\n7,8\n2\n");
}

// The `break` edge: the enumerator is disposed with the loop variable still
// live, so the cell must already have been released by the unbox — the
// borrowing (value-T) variant's Dispose frees `current_yielded` only for
// `is_owning`, and this path is `is_owning == 0`, so the decoder's release is
// the only one. Leaking OR double-freeing here is a bug; LSan/ASan name which.
BEZEL_TEST(channel_record_element_5305, receive_all_break_releases_cell) {
    expectClean("module M; import System.Threading; " + std::string(kP) +
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<P>(); "
                "  var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 50 do tr.Send(new P(i, i * 2)); i := i + 1; end; "
                "  do using var t := move tr; end; "
                "  await for var v in cr.ReceiveAll() do "
                "    if v.X = 3 then Console.WriteLine($\"{v.X},{v.Y}\"); break; end; "
                "  end; "
                "  return 0; end Main; end C;",
                "3,6\n");
}

// A drain whose elements each carry a `string` field, over 200 items: the cell
// holds a byte-copy of the record, so this row pins that the shared-reference
// `string` survives the copy (an imbalance shows as an LSan leak or a
// use-after-free) AND that no cell outlives its iteration.
BEZEL_TEST(channel_record_element_5305, receive_all_record_with_string_field) {
    expectClean("module M; import System.Threading; record P(S: string, N: int); "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<P>(); "
                "  var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 200 do "
                "    tr.Send(new P(\"payload-value\", i)); i := i + 1; "
                "  end; "
                "  do using var t := move tr; end; "
                "  var acc := 0; var n := 0; "
                "  await for var v in cr.ReceiveAll() do "
                "    if v.S = \"payload-value\" then acc := acc + v.N; end; "
                "    n := n + 1; "
                "  end; "
                "  Console.WriteLine(acc); Console.WriteLine(n); "
                "  return 0; end Main; end C;",
                "19900\n200\n");
}

// =====================================================================
// CONTROL ROWS — the element kinds that were already correct. These must
// stay green in EVERY leg, pre-fix and post-fix: they are what makes a
// FAIL-WITHOUT on a user row attributable to this fix and not to a change
// in the shared codec.
// =====================================================================

namespace {

std::string ctl(const std::string& t, const std::string& sendExpr,
                const std::string& show) {
    return "module M; class C public static method Main(): int "
           "  using var cr := new Channel<" + t + ">(); "
           "  using var tr := cr.NewSender(); "
           "  tr.Send(" + sendExpr + "); "
           "  var o: " + t + "? := null; "
           "  if cr.TryReceive(out o) then Console.WriteLine(" + show + "); end; "
           "  return 0; end Main; end C;";
}

}  // namespace

BEZEL_TEST(channel_record_element_5305, control_int) {
    expect(ctl("int", "42", "o!"), "42\n");
}

BEZEL_TEST(channel_record_element_5305, control_long) {
    expect(ctl("long", "1234567890123L", "o!"), "1234567890123\n");
}

BEZEL_TEST(channel_record_element_5305, control_double) {
    expect(ctl("double", "2.5", "o! * 2.0"), "5\n");
}

BEZEL_TEST(channel_record_element_5305, control_bool) {
    expect(ctl("bool", "true", "o!"), "True\n");
}

BEZEL_TEST(channel_record_element_5305, control_string) {
    expect(ctl("string", "\"abc\"", "o!"), "abc\n");
}

BEZEL_TEST(channel_record_element_5305, control_enum) {
    expect("module M; enum Col Red, Green; end Col; "
           "class C public static method Main(): int "
           "  using var cr := new Channel<Col>(); "
           "  using var tr := cr.NewSender(); "
           "  tr.Send(Col.Green); "
           "  var o: Col? := null; "
           "  if cr.TryReceive(out o) then "
           "    if o! = Col.Green then Console.WriteLine(\"green\"); end; "
           "  end; "
           "  return 0; end Main; end C;",
           "green\n");
}

// The class element is the owning shape: the item IS the object pointer, and
// the receiver's `using` binding disposes it. Unchanged by this fix.
BEZEL_TEST(channel_record_element_5305, control_sealed_class) {
    expectClean("module M; sealed class It "
                "  public V: int := 0; "
                "  public constructor(v: int) V := v; end constructor; "
                "end It; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<It>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(new It(11)); "
                "  var o: It? := null; "
                "  if cr.TryReceive(out o) then "
                "    using var j := move o!; "
                "    Console.WriteLine(j.V); "
                "  end; "
                "  return 0; end Main; end C;",
                "11\n");
}

// Regression test for #5304 / stdlib §6.9 / §28.11 / §28.9: a Channel<string>
// leaks every undelivered string at channel disposal because
// elementDisposerArg(string, owning) returns a null ConstantPointerNull
// (RuntimeManaged row: no disposer), so bz_channel_dispose drains the deque
// WITHOUT calling bz_string_release on each buffered item — every string
// header+data leaks. 200 iterations pin the leak (one-shot does not reach
// LSan threshold). The interpolation $"" must be used (a string LITERAL
// carries no heap allocation and would not be detected).
//
// THE FIX: the isAggregateChannelElement null-vs-thunk override in lowerNew
// used `!dispArg` which is always false for an llvm::Value* (the Value is
// never a null pointer; it is a ConstantPointerNull at most).  The correct
// check is llvm::isa<llvm::ConstantPointerNull>(dispArg).
//
// The TRYRECEIVE arm is separate (#5304 original): the Send lowering emits
// bz_string_retain when the channel takes ownership, so TryReceive must
// release the channel's reference when handing the string to the caller.
// That fix (emitStringRelease in TryReceive) was already on r23-c28.
BEZEL_TEST(channel_record_element_5305, channel_string_undelivered_leak_5304) {
    // 200 iterations with no receive: all items are undelivered at channel
    // disposal. exitCode==0 + no LSan report = no leak.
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<string>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 200 do "
                "    var s: string := $\"payload-{i}\"; "
                "    tr.Send(s); "
                "    i := i + 1; "
                "  end; "
                "  return 0; end Main; end C;",
                // No output expected; the test asserts exitCode == 0 (no leak)
                "");
}

// Regression tests for #5305-ff2 — the `Channel<[N]T>` Send/TryReceive
// defect and the predicate drift that caused it.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   Spec §30.14 (v0.313), value-type bullet: "All primitives and enums are
//   sendable; a **record** is sendable iff it is not borrow-bearing and every
//   field's type is sendable, an **inline array** iff its element type is
//   sendable, and a **choice** per §35.12 ... A sendable value crosses as a
//   copy." The inline-array clause is the one this file exercises: `[N]T` is
//   a §30.14-sendable value type, so `Channel<[3]int>` is well-formed.
//   Spec §30.15 (v0.313): "A **channel** carries sendable values from one
//   domain to another ... their element type must be sendable."
//   Spec §15.1: an inline array's storage is inline — its §2.8.3 lowering is
//   an LLVM ARRAY type, `[N x T]`, not a pointer.
//   Stdlib §6.9: `Channel<T> where T: sendable`; for sendable value-typed T,
//   `Send(item: T)`, `TryReceive(out item: T?): bool`, `Receive(): Task<T>`,
//   `ReceiveAll(): IAsyncEnumerable<T>`.
//
// THE DEFECT. The channel carries ONE ptr-sized word per item. A scalar rides
// that word directly; a value that does not fit in a word must ride it as a
// POINTER to a fresh heap cell holding a copy (§30.14's "crosses as a copy").
// Four sites had to agree on which elements are such a value, and they did
// not:
//
//   site                              pre-#5305-ff2 gate
//   -------------------------------  ---------------------------------------
//   encoder (Calls3 encodeChannel..)  `ity->isAggregateType()`  (lowered LLVM)
//   decoder  (Calls3 TryReceive)      `outEty->isStructTy()`    (out-slot LLVM)
//   decoder  (Async await Receive)    `sema::isAggregateChannelElement`
//   decoder  (Stmts ReceiveAll drain) `sema::isAggregateChannelElement`
//   disposer  (Exprs2 element free)   `sema::isAggregateChannelElement`
//   sema admission (Sendability:246)  admitted the inline array
//
// `sema::isAggregateChannelElement` — the ONE predicate three of the five
// sites already used — read `Record || Choice` and did NOT include
// `InlineArray`, so it contradicted sema's own admission at
// Sendability.cpp:246. For a `Channel<[3]int>`: the encoder saw an LLVM ARRAY
// and boxed it, while the `await Receive` and `ReceiveAll` decoders asked the
// predicate, were told "scalar", and read the raw cell ADDRESS as the
// element's bytes; the TryReceive decoder asked the out-slot's LLVM type
// (`[3 x i32]`, not a struct), fell to its final `else`, and stored the raw
// cell pointer into the out-slot. MEASURED pre-fix: `await` and `ReceiveAll`
// printed the cell address; `TryReceive` printed the first two slots correct
// (the address's low bytes) and the rest garbage — silently wrong, no
// diagnostic, and a leak of every boxed cell on the drain paths.
//
// THE FIX. `sema::isAggregateChannelElement` is the single predicate, widened
// so that it is correct for EVERY element sema admits, and the encoder, both
// decoders and the disposer all call it with the SAME element `TypeRef` —
// threaded through the codec rather than re-derived from the lowered LLVM type
// at each site. "The sites cannot drift" is then a structural claim, not a
// comment: no site asks a second question.
//
// The widening is a SUPERSET of the integrator's `Record || Choice ||
// InlineArray`, and the extra members are not discretionary — each was
// measured. `decimal` (§4.4) lowers to the `%bz.decimal` STRUCT, and with
// `decimal` outside the set the pre-fix encoder's LLVM gate put it down the
// scalar arm; routing it through the predicate instead would have been an
// IR-verification abort, so leaving it out would REGRESS a working program.
// A NULLABLE VALUE type of any element kind (`int?`, `Col?`, `P?`,
// `decimal?`) lowers per §7.11 to the tagged `{i8 hasValue; T}` struct, so it
// too is an aggregate. See the per-member evidence in
// `src/sema/Sendability.cpp` at the definition.
//
// ROWS. Thirteen USER rows, each MEASURED to fail against the base
// (c32d8f08) and to pass with the fix: the ten `inline_array_*` rows
// (§15.1 / §30.14 / §30.15) plus the three that cover the OTHER members the
// predicate had to be widened to — `decimal` through the `await` decoder
// (§4.4) and a nullable value type through both decoders (§7.11). Those three
// are the evidence that the widening is a superset and not a swap: they were
// silently wrong at the base for exactly the reason an inline array was.
//
// The CONTROL rows are green in EVERY leg, base and fixed, and are what make
// a FAIL-WITHOUT on a user row attributable to this fix rather than to a
// change in the shared codec: the record and choice arms the base already got
// right, every scalar kind through both decoders, the class element, a record
// HOLDING an inline array (a different shape from an inline array AS the
// element — the record is the aggregate there, the array rides in its bytes),
// and the two struct-lowering non-members `Span`/`Range`, pinned by the
// diagnostic that rejects them.
//
// Naming: `*_spec_<section>` names the section the row is derived from; the
// suite name carries the issue. Every row is `runAotChecked` (generated code
// under ASan + LSan), because a cell leak and a double free are the failure
// modes this fix is about and neither is visible in exit code.

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

// Compile + run under generated-code ASan + LSan, asserting the exact stdout,
// a clean exit, and (via `runAotChecked`) that the generated program's own
// leak check was clean. The exact-stdout oracle is what makes a "silently
// wrong value" decode fail: printing the received slots catches a cell
// ADDRESS read as the element's bytes.
void expectClean(const std::string& src, const std::string& want) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;   // ASan toolchain unavailable — infra skip
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// Assert a compile-time rejection by its diagnostic text. Used only for the
// non-sendable element kinds: if admission ever widened to admit them, this
// row is what would notice.
void expectRejected(const std::string& src, const std::string& needle) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    BEZEL_EXPECT_TRUE(all.find(needle) != std::string::npos);
}

// `record P(X: int, Y: int);` — the sibling file's all-value element, for the
// control rows that must be unaffected.
const char* kP = "record P(X: int, Y: int); ";

}  // namespace

// =====================================================================
// USER ROWS — the `Channel<[N]T>` defect. Each FAILS pre-fix.
// =====================================================================

// §30.14 / §30.15: the inline-array sendable clause, end to end. 200
// send/receive pairs, each value checked slot by slot — a decode that returned
// the cell address passes on zero of the 200 comparisons, so this is not a
// one-shot-oracle row. Pre-fix: `TryReceive` stores the raw cell pointer into
// the `[3 x i32]` out-slot, so the comparison fails and every cell leaks.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_tryreceive_spec_30_14) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; var acc := 0; var n := 0; "
                "  while i < 200 do "
                "    var a: [3]int := [i, i * 2, i * 3]; "
                "    tr.Send(a); "
                "    var o: [3]int := [0, 0, 0]; "
                "    if cr.TryReceive(out o) then "
                "      if o[0] = i and o[1] = i * 2 and o[2] = i * 3 then "
                "        acc := acc + 1; "
                "      end; "
                "      n := n + 1; "
                "    end; "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(acc); Console.WriteLine(n); "
                "  Console.WriteLine(cr.Count()); "
                "  return 0; end Main; end C;",
                "200\n200\n0\n");
}

// §30.14: an inline array of `string` — an element type that OWNS heap, so the
// boxed cell is a shallow byte-copy holding live string references. This is
// the row that pins "no leak, no double free" on the owned-element path: a
// leaked cell and a double-freed cell are both invisible in the exit code.
// The elements are literals (a string constant, no heap reference); the row's
// subject is the CELL, not `string` disposal. See NOT-IN-SCOPE below for why
// a heap-allocated `string` element is not used here.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_of_string_no_leak_spec_30_14) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[2]string>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; var acc := 0; var n := 0; "
                "  while i < 200 do "
                "    var s: [2]string := [\"ab\", \"cd\"]; "
                "    tr.Send(s); "
                "    var o: [2]string := [\"\", \"\"]; "
                "    if cr.TryReceive(out o) then "
                "      if o[0] = \"ab\" then acc := acc + 1; end; "
                "      if o[1] = \"cd\" then n := n + 1; end; "
                "    end; "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(acc); Console.WriteLine(n); "
                "  return 0; end Main; end C;",
                "200\n200\n");
}

// §30.15 / stdlib §6.9: `await Receive()` reads the item word out of the task's
// result buffer. Pre-fix this decoder asked `isAggregateChannelElement`, was
// told "scalar", and read the cell ADDRESS as the element's bytes — the row
// printed a raw pointer where `1,2,3` belonged.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_await_receive_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var a: [3]int := [1, 2, 3]; "
                "  tr.Send(a); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  return 0; end Main; end C;",
                "1,2,3\n");
}

// §30.15: `ChannelSender<T>.SendAsync` hands the SAME item word to the SAME
// runtime entry as the sync `Send`, so it is one of the two Send sites this
// fix re-gates. Paired with `TryReceive` to check the value that crossed.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_send_async_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var a: [3]int := [4, 5, 6]; "
                "  await tr.SendAsync(a); "
                "  var o: [3]int := [0, 0, 0]; "
                "  if cr.TryReceive(out o) then "
                "    Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  end; "
                "  return 0; end Main; end C;",
                "4,5,6\n");
}

// §30.15: `ReceiveAll()` yields "each item with `Receive`'s transfer
// semantics". The drain's element handoff returns the channel's item word, so
// the loop variable needs the same unbox. Pre-fix this decoder asked the
// predicate, was told "scalar", and printed the cell address — and leaked
// every cell it printed, since nothing released them.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_receive_all_drain_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 100 do "
                "    var a: [3]int := [i, i * 2, i * 3]; "
                "    tr.Send(a); i := i + 1; "
                "  end; "
                "  do using var t := move tr; end; "     // drop sender => ch closes
                "  var n := 0; "
                "  await for var v in cr.ReceiveAll() do "
                "    Console.WriteLine($\"{v[0]},{v[1]},{v[2]}\"); n := n + 1; "
                "  end; "
                "  Console.WriteLine(n); "
                "  return 0; end Main; end C;",
                "0,0,0\n1,2,3\n2,4,6\n3,6,9\n4,8,12\n5,10,15\n6,12,18\n"
                "7,14,21\n8,16,24\n9,18,27\n10,20,30\n11,22,33\n12,24,36\n"
                "13,26,39\n14,28,42\n15,30,45\n16,32,48\n17,34,51\n18,36,54\n"
                "19,38,57\n20,40,60\n21,42,63\n22,44,66\n23,46,69\n24,48,72\n"
                "25,50,75\n26,52,78\n27,54,81\n28,56,84\n29,58,87\n30,60,90\n"
                "31,62,93\n32,64,96\n33,66,99\n34,68,102\n35,70,105\n"
                "36,72,108\n37,74,111\n38,76,114\n39,78,117\n40,80,120\n"
                "41,82,123\n42,84,126\n43,86,129\n44,88,132\n45,90,135\n"
                "46,92,138\n47,94,141\n48,96,144\n49,98,147\n50,100,150\n"
                "51,102,153\n52,104,156\n53,106,159\n54,108,162\n"
                "55,110,165\n56,112,168\n57,114,171\n58,116,174\n"
                "59,118,177\n60,120,180\n61,122,183\n62,124,186\n"
                "63,126,189\n64,128,192\n65,130,195\n66,132,198\n"
                "67,134,201\n68,136,204\n69,138,207\n70,140,210\n"
                "71,142,213\n72,144,216\n73,146,219\n74,148,222\n"
                "75,150,225\n76,152,228\n77,154,231\n78,156,234\n"
                "79,158,237\n80,160,240\n81,162,243\n82,164,246\n"
                "83,166,249\n84,168,252\n85,170,255\n86,172,258\n"
                "87,174,261\n88,176,264\n89,178,267\n90,180,270\n"
                "91,182,273\n92,184,276\n93,186,279\n94,188,282\n"
                "95,190,285\n96,192,288\n97,194,291\n98,196,294\n"
                "99,198,297\n100\n");
}

// §30.15: the `break` edge. The drain enumerator is disposed with the loop
// variable still live and 95 cells still buffered, so the buffered cells must
// be released by the channel's disposal and the yielded one by the decoder's
// own release. Leaking OR double-freeing here is a bug; LSan/ASan name which.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_receive_all_break_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 100 do "
                "    var a: [3]int := [i, i * 2, i * 3]; "
                "    tr.Send(a); i := i + 1; "
                "  end; "
                "  do using var t := move tr; end; "
                "  await for var v in cr.ReceiveAll() do "
                "    if v[0] = 3 then Console.WriteLine($\"{v[0]},{v[1]},{v[2]}\"); break; end; "
                "  end; "
                "  return 0; end Main; end C;",
                "3,6,9\n");
}

// §30.15 / §17.13: the `out _` discard. No binding survives, so the received
// cell must still be released — 200 of them, or the leak is unmissable.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_out_discard_releases_cell_spec_30_15) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 200 do "
                "    var a: [3]int := [i, i, i]; "
                "    tr.Send(a); "
                "    cr.TryReceive(out _); "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(cr.Count()); "
                "  return 0; end Main; end C;",
                "0\n");
}

// §30.15: a cell still buffered when the channel is disposed must be released
// by the channel's element disposer — which is the FIFTH site this fix
// re-gates, and the one that shares the predicate for release rather than for
// encoding.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_undrained_cells_released_spec_30_15) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var i := 0; "
                "  while i < 100 do "
                "    var a: [3]int := [i, i, i]; "
                "    tr.Send(a); i := i + 1; "
                "  end; "
                "  Console.WriteLine(cr.Count()); "
                "  return 0; end Main; end C;",
                "100\n");
}

// §30.15 / stdlib §6.9: a send REFUSED because a bounded channel is full
// still made a cell, which the refusal path must release ("disposing the
// offered item first"). The item that DID get in must still decode correctly.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_full_channel_refused_send_spec_30_15) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[3]int>(1); "
                "  using var tr := cr.NewSender(); "
                "  var a: [3]int := [1, 2, 3]; "
                "  var b: [3]int := [7, 8, 9]; "
                "  tr.Send(a); "
                "  try "
                "    tr.Send(b); "
                "    Console.WriteLine(\"no-exc\"); "
                "  catch e: ChannelFullException => "
                "    Console.WriteLine(\"full\"); "
                "  end; "
                "  var o: [3]int := [0, 0, 0]; "
                "  if cr.TryReceive(out o) then "
                "    Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  end; "
                "  return 0; end Main; end C;",
                "full\n1,2,3\n");
}

// §30.15 / stdlib §6.9: the same obligation on the CLOSED refusal path
// ("disposing the offered item first"). No `out` binding survives here, so the
// only thing that can catch a leaked cell is LSan.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           inline_array_closed_channel_refused_send_spec_30_15) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<[3]int>(); "
                "  do using var t1 := cr.NewSender(); end; "
                "  using var t2 := cr.NewSender(); "
                "  var i := 0; var refused := 0; "
                "  while i < 100 do "
                "    var a: [3]int := [i, i, i]; "
                "    try "
                "      t2.Send(a); "
                "      Console.WriteLine(\"no-exc\"); "
                "    catch e: ChannelClosedException => "
                "      refused := refused + 1; "
                "    end; "
                "    i := i + 1; "
                "  end; "
                "  Console.WriteLine(refused); "
                "  return 0; end Main; end C;",
                "100\n");
}

// =====================================================================
// CONTROL ROWS — the shapes that were already correct, and must stay
// correct in EVERY leg, pre-fix and post-fix. They are what makes a
// FAIL-WITHOUT on a user row attributable to this fix and not to a change
// in the shared codec.
// =====================================================================

// §30.14 / §15.1: an inline array held BY A RECORD is a different shape from
// an inline array as the ELEMENT. The record is the aggregate (it lowered to a
// struct pre-fix, so the encoder and the TryReceive decoder already agreed);
// the array rides inside the cell's bytes. This row is green in every leg and
// its staying green is the evidence that the widening did not disturb the
// record arm. Named for the issue's reviewer repro.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_record_holding_inline_array_spec_30_14) {
    expectClean("module M; record P(S: [1]string, N: int); "
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(new P([\"hello\"], 7)); "
                "  var o: P? := null; "
                "  if cr.TryReceive(out o) then "
                "    Console.WriteLine(o!.S[0]); Console.WriteLine(o!.N); "
                "  end; "
                "  return 0; end Main; end C;",
                "hello\n7\n");
}

// §30.14: the same shape with an all-value array member, so the row does not
// depend on `string` reference behaviour to tell the two arms apart.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_record_holding_int_array_spec_30_14) {
    expectClean("module M; record P(A: [3]int, N: int); "
                "class C public static method Main(): int "
                "  using var cr := new Channel<P>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(new P([4, 5, 6], 7)); "
                "  var o: P? := null; "
                "  if cr.TryReceive(out o) then "
                "    Console.WriteLine(o!.A[0] + o!.A[1] + o!.A[2]); "
                "    Console.WriteLine(o!.N); "
                "  end; "
                "  return 0; end Main; end C;",
                "15\n7\n");
}

// §4.4: `decimal` is the one PRIMITIVE that lowers to a struct
// (`%bz.decimal`). It was correct before this fix only because the ENCODER
// asked the lowered LLVM type while the OTHER sites asked the predicate — the
// drift itself, in the one direction that happened to work. With the encoder
// re-gated on the predicate, `decimal` must be IN it or this row regresses to
// an IR-verification abort. Its staying green is the evidence that the
// widening was a superset and not a swap.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_decimal_element_spec_4_4) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<decimal>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(1.5m); "
                "  var o: decimal? := null; "
                "  if cr.TryReceive(out o) then Console.WriteLine(o!); end; "
                "  return 0; end Main; end C;",
                "1.5\n");
}

// §4.4: the same obligation on the `await Receive` decode, which at the base
// asked the predicate and was therefore told "scalar" — it read the cell
// address as the decimal's bytes, a 16-byte heap-buffer-overflow under ASan.
// MEASURED failing at c32d8f08, and the row that shows why `decimal` had to
// be ADDED to the predicate rather than merely not-removed: re-gating the
// encoder on the base predicate would have sent it down the scalar arm.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           decimal_await_receive_spec_4_4) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<decimal>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(1.5m); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o); "
                "  return 0; end Main; end C;",
                "1.5\n");
}

// §7.11: a NULLABLE VALUE type lowers to the tagged `{i8 hasValue; T}` struct,
// so `int?` is an aggregate by the same reasoning as an inline array — and at
// the base it was NOT in the predicate, so the encoder boxed it (on the LLVM
// type) while the `await` decoder read the cell address as its bytes.
// MEASURED failing at c32d8f08. A USER row: it is one of the members the
// predicate had to be widened to.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           nullable_value_await_receive_spec_7_11) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<int?>(); "
                "  using var tr := cr.NewSender(); "
                "  var v: int? := 42; "
                "  tr.Send(v); "
                "  var o := await cr.Receive(); "
                "  if o <> null then Console.WriteLine(o!); end; "
                "  return 0; end Main; end C;",
                "42\n");
}

// §7.11: the same nullable element through `TryReceive`, whose new aggregate
// arm has two sub-cases — the out-slot is the TAGGED `{i8 hasValue; T}` struct
// (the record/decimal shape) or it IS the element's own lowering (this shape,
// and an inline array's). Only the second sub-case is exercised by an inline
// array, so it gets its own row here.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           nullable_value_try_receive_spec_7_11) {
    expectClean("module M; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<int?>(); "
                "  using var tr := cr.NewSender(); "
                "  var v: int? := 42; "
                "  tr.Send(v); "
                "  var o: int? := null; "
                "  if cr.TryReceive(out o) then "
                "    if o <> null then Console.WriteLine(o!); end; "
                "  end; "
                "  return 0; end Main; end C;",
                "42\n");
}

// §35.12: the third member of the predicate's union. A choice was already a
// member, and the sibling file `channel_record_element_5305_test.cpp` carries
// its own exhaustive rows; this one exists because a three-way union in which
// only the new member is exercised is exactly how the two-way union was
// allowed to drift in the first place.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_choice_element_spec_35_12) {
    expectClean("module M; choice Sh A(i: int), B(s: string); end Sh; "
                "class C public static method Main(): int "
                "  using var cr := new Channel<Sh>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(Sh.A(9)); "
                "  var o: Sh? := null; "
                "  if cr.TryReceive(out o) then "
                "    case o! of "
                "      A(var n) => do Console.WriteLine(n); end, "
                "      B(var t) => do Console.WriteLine(t); end "
                "    end; "
                "  end; "
                "  return 0; end Main; end C;",
                "9\n");
}

namespace {

// The ordinary element kinds, green in every leg. `ctl` is the sibling file's
// helper, kept local to this translation unit.
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

BEZEL_TEST(channel_inline_array_element_5305ff2, control_int_spec_30_14) {
    expectClean(ctl("int", "42", "o!"), "42\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2, control_long_spec_30_14) {
    expectClean(ctl("long", "1234567890123L", "o!"), "1234567890123\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2, control_double_spec_30_14) {
    expectClean(ctl("double", "2.5", "o! * 2.0"), "5\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2, control_bool_spec_30_14) {
    expectClean(ctl("bool", "true", "o!"), "True\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2, control_string_spec_30_14) {
    expectClean(ctl("string", "\"abc\"", "o!"), "abc\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2, control_enum_spec_30_14) {
    expectClean("module M; enum Col Red, Green; end Col; "
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

// §30.15: the class element is the owning shape — the item IS the object
// pointer and the receiver's `using` binding disposes it. Widening the
// aggregate set must not have swept the reference forms into it.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_sealed_class_element_spec_30_15) {
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

// §30.15: the scalar arm of the `await Receive` decoder, one row per element
// kind, so a predicate widened the other way — by lowering everything and
// asking the LLVM type — fails on a specific kind rather than on a composite
// oracle.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_int_await_receive_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<int>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(42); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o); "
                "  return 0; end Main; end C;",
                "42\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_long_await_receive_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<long>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(1234567890123L); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o); "
                "  return 0; end Main; end C;",
                "1234567890123\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_double_await_receive_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<double>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(1.25); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o); "
                "  return 0; end Main; end C;",
                "1.25\n");
}

BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_string_await_receive_spec_30_15) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<string>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send(\"abc\"); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine(o); "
                "  return 0; end Main; end C;",
                "abc\n");
}

// §30.14: the struct-lowering NON-members. `Span`/`Range`/`Guid`/`DateTime`
// lower to LLVM structs, which is exactly why the pre-fix encoder's
// `isAggregateType()` gate was a hazard rather than a shortcut — but sema
// rejects all of them as not sendable, so the predicate never sees them.
// These rows pin the rejection by its diagnostic text: if admission ever
// widened, this is what would notice, and it would notice BEFORE a box/free
// pair could disagree about them.
BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_span_element_rejected_spec_30_14) {
    expectRejected("module M; "
                   "class C public static method Main(): int "
                   "  using var cr := new Channel<Span<int>>(); "
                   "  return 0; end Main; end C;",
                   "is not sendable");
}

BEZEL_TEST(channel_inline_array_element_5305ff2,
           control_range_element_rejected_spec_30_14) {
    expectRejected("module M; "
                   "class C public static method Main(): int "
                   "  using var cr := new Channel<Range<int>>(); "
                   "  return 0; end Main; end C;",
                   "is not sendable");
}

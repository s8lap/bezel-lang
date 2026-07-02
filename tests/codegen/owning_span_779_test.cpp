// Codegen + sema — #779 / #870 / #907: the OWNING `Span<T>` over an
// `OwningBuffer<T>` (stdlib §4.14), distinct from `Array<T>`'s borrowing
// `ArraySpan<T>` (§4.15).
//
// Root cause (pre-fix): `OwningBuffer<T>.AsSpan()` was mis-typed as the borrowing
// `ArraySpan<T>` (the #636 shortcut). The owning span's class-typed slots OWN
// their elements, so the borrowing typing produced three defects:
//   #870  `OwningBuffer<classT>.AsSpan().Fill(c)` was accepted (ArraySpan has
//         Fill for all T) and SIGSEGV'd — one borrowed ref aliased into N owning
//         slots → double-free. On an owning `Span<classT>` Fill is forbidden.
//   #907  `move sp[i]` had no codegen branch for the owning span → it fell to a
//         plain read that did not null the buffer slot → double-free at the
//         buffer's scope-end disposal.
//   #779  `sp[i] := v` went through the non-disposing borrowing-set → the
//         displaced owned element LEAKED; and Clear/MoveFrom/MoveTo were absent.
//
// The fix retypes `OwningBuffer<T>.AsSpan()` → owning `Span<T>` (§4.14): a
// disposing indexer-set, an admitted `move sp[i]`, the class-T Clear/MoveFrom/
// MoveTo intrinsics, and a sema reject of Fill/CopyFrom/CopyTo on class-T. The
// value-typed Span keeps the value-copy surface (Fill/CopyFrom/CopyTo); the
// borrowing ArraySpan over Array<T> is unchanged.
//
// Leak / double-free is detected with a static instance counter `R.n` (ctor +1,
// destructor -1); a final value of 0 means every instance was disposed exactly
// once (no leak, no double-free, no negative count).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejectedAtCompile(const std::string& prog) {
    auto r = runJit(prog);
    return !r.ran && r.diagnostics.hasErrors();
}
// R counts live instances; V carries a payload.
const char* kR =
    "class R public static n: int := 0; public V: int; "
    "  public constructor(v: int) R.n := R.n + 1; V := v; end constructor; "
    "  destructor R.n := R.n - 1; end destructor; end R; ";
}  // namespace

// ===========================================================================
// #779 — AsSpan() returns the owning Span<T>; basic shape + Length.
// ===========================================================================
BEZEL_TEST(owning_span_779, asspan_length_class_element) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<R>(4); "
        "  var sp := buf.AsSpan(); "
        "  return sp.Length; "  // 4
        "end Main; end C;"), 4);
}

// ===========================================================================
// #779 — the owning span's indexer-set DISPOSES the displaced element.
// buf owns Obj(1),Obj(2); sp[0] := Obj(3) must dispose the displaced Obj(1).
// 3 created; Obj(1) disposed at the set, Obj(2)+Obj(3) disposed at buf's
// scope end → R.n returns to 0 (pre-fix the displaced Obj(1) leaked → 1).
// ===========================================================================
BEZEL_TEST(owning_span_779, indexer_set_disposes_displaced_element) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var buf := new OwningBuffer<R>(2); "
        "    buf[0] := new R(1); buf[1] := new R(2); "
        "    var sp := buf.AsSpan(); "
        "    sp[0] := new R(3); "
        "  end; "
        "  return R.n; "  // 0
        "end Main; end C;"), 0);
}

// The new value read back through the span is the freshly-stored one.
BEZEL_TEST(owning_span_779, indexer_set_then_get_reads_new_value) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<R>(2); "
        "  buf[0] := new R(1); "
        "  var sp := buf.AsSpan(); "
        "  sp[0] := new R(9); "
        "  return sp[0]!.V; "  // 9 — sp[i] is R? (#1208), narrow with `!`
        "end Main; end C;"), 9);
}

// ===========================================================================
// #907 — `move sp[i]` on the owning span transfers the owned element out and
// leaves the buffer slot moved-from (null). The moved value is correct AND the
// buffer's scope-end disposal must skip the nulled slot (no double-free).
// buf owns R(10),R(11); `using var t := move sp[0]` takes R(10): t disposed
// once at its scope end, buf disposes only slot[1] (R(11)) → R.n → 0.
// ===========================================================================
BEZEL_TEST(owning_span_779, move_slot_value_correct) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<R>(2); "
        "  buf[0] := new R(10); buf[1] := new R(11); "
        "  var sp := buf.AsSpan(); "
        "  using var t := move sp[0]; "
        "  return t!.V; "  // 10 — move sp[i] yields R? (#1208, like move buf[i])
        "end Main; end C;"), 10);
}

BEZEL_TEST(owning_span_779, move_slot_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var buf := new OwningBuffer<R>(2); "
        "    buf[0] := new R(10); buf[1] := new R(11); "
        "    var sp := buf.AsSpan(); "
        "    using var t := move sp[0]; "
        "  end; "
        "  return R.n; "  // 0 — each of the two instances disposed exactly once
        "end Main; end C;"), 0);
}

// ===========================================================================
// #779 — Clear() disposes every slot and leaves them null (no double-free at
// scope end). buf owns R(1),R(2); after Clear both are disposed, the buffer
// skips the nulled slots → R.n → 0.
// ===========================================================================
BEZEL_TEST(owning_span_779, clear_disposes_all_slots) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var buf := new OwningBuffer<R>(2); "
        "    buf[0] := new R(1); buf[1] := new R(2); "
        "    var sp := buf.AsSpan(); "
        "    sp.Clear(); "
        "  end; "
        "  return R.n; "  // 0
        "end Main; end C;"), 0);
}

// ===========================================================================
// #779 — MoveFrom moves ownership slot-by-slot, disposing the destination's
// previous contents and nulling the source. Two buffers a (R(1),R(2)) and
// b (R(3),R(4)); b.AsSpan().MoveFrom(a.AsSpan()): b's R(3),R(4) disposed, b now
// holds R(1),R(2), a's slots nulled. Final value read from b, and counts.
// ===========================================================================
BEZEL_TEST(owning_span_779, movefrom_transfers_and_disposes_dest) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var a := new OwningBuffer<R>(2); "
        "  using var b := new OwningBuffer<R>(2); "
        "  a[0] := new R(1); a[1] := new R(2); "
        "  b[0] := new R(3); b[1] := new R(4); "
        "  var sa := a.AsSpan(); var sb := b.AsSpan(); "
        "  sb.MoveFrom(sa); "
        "  return sb[0]!.V + sb[1]!.V; "  // 1 + 2 == 3 (sb[i] is R?, #1208)
        "end Main; end C;"), 3);
}

// MoveFrom disposes exactly once overall: R(3),R(4) (displaced) disposed at the
// move, R(1),R(2) live in b and disposed at b's scope end, a's slots are null →
// R.n returns to 0 (4 created, 4 disposed, no double-free).
BEZEL_TEST(owning_span_779, movefrom_no_leak_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new OwningBuffer<R>(2); "
        "    using var b := new OwningBuffer<R>(2); "
        "    a[0] := new R(1); a[1] := new R(2); "
        "    b[0] := new R(3); b[1] := new R(4); "
        "    var sa := a.AsSpan(); var sb := b.AsSpan(); "
        "    sb.MoveFrom(sa); "
        "  end; "
        "  return R.n; "  // 0
        "end Main; end C;"), 0);
}

// MoveTo is the symmetric form (a.MoveTo(b) == b.MoveFrom(a)).
BEZEL_TEST(owning_span_779, moveto_symmetric) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var a := new OwningBuffer<R>(2); "
        "  using var b := new OwningBuffer<R>(2); "
        "  a[0] := new R(5); a[1] := new R(6); "
        "  b[0] := new R(7); b[1] := new R(8); "
        "  var sa := a.AsSpan(); var sb := b.AsSpan(); "
        "  sa.MoveTo(sb); "
        "  return sb[0]!.V + sb[1]!.V; "  // 5 + 6 == 11 (sb[i] is R?, #1208)
        "end Main; end C;"), 11);
}

// ===========================================================================
// #870 — Fill / CopyFrom / CopyTo are REJECTED on an owning Span<classT>
// (they would alias one borrowed reference into multiple owning slots).
// ===========================================================================
BEZEL_TEST(owning_span_779, fill_on_class_span_rejected_870) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<R>(2); "
        "  using var c := new R(1); "
        "  var sp := buf.AsSpan(); "
        "  sp.Fill(c); "
        "  return 0; "
        "end Main; end C;"), true);
}

// ===========================================================================
// Value-typed Span keeps the value-copy surface: Fill works on Span<int>.
// ===========================================================================
BEZEL_TEST(owning_span_779, fill_on_value_span_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(3); "
        "  var sp := buf.AsSpan(); "
        "  sp.Fill(7); "
        "  return buf[0] + buf[1] + buf[2]; "  // 21
        "end Main; end C;"), 21);
}

// `move sp[i]` is NOT admitted on a value-typed Span (§4.14): plain read/copy.
BEZEL_TEST(owning_span_779, move_on_value_span_rejected) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(2); "
        "  var sp := buf.AsSpan(); "
        "  var x := move sp[0]; "
        "  return x; "
        "end Main; end C;"), true);
}

// Clear is NOT admitted on a value-typed Span (use Fill instead).
BEZEL_TEST(owning_span_779, clear_on_value_span_rejected) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(2); "
        "  var sp := buf.AsSpan(); "
        "  sp.Clear(); "
        "  return 0; "
        "end Main; end C;"), true);
}

// ===========================================================================
// AsReadOnly() yields a ReadOnlySpan<T>; writing through it is rejected.
// ===========================================================================
BEZEL_TEST(owning_span_779, readonly_span_set_rejected) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<R>(2); "
        "  buf[0] := new R(1); "
        "  var ro := buf.AsReadOnlySpan(); "
        "  ro[0] := new R(2); "
        "  return 0; "
        "end Main; end C;"), true);
}

// ===========================================================================
// Regression — Array<T>'s borrowing ArraySpan is UNCHANGED: Fill still works
// for all T (its slots are borrows, not owners).
// ===========================================================================
BEZEL_TEST(owning_span_779, array_arrayspan_fill_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var arr := new Array<int>(); "
        "  arr.Add(0); arr.Add(0); arr.Add(0); "
        "  var sp := arr.AsSpan(); "
        "  sp.Fill(5); "
        "  return arr[0] + arr[1] + arr[2]; "  // 15
        "end Main; end C;"), 15);
}

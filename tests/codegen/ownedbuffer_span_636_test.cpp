// Codegen + sema — #636: OwningBuffer<T>.AsSpan() / AsReadOnlySpan() lowering.
//
// Root cause (pre-fix):
//   isSerializationCall() intercepted OwningBuffer<byte>.AsSpan / AsReadOnlySpan
//   BEFORE tryLowerArraySpanCall could see them, routing them to a duplicate
//   handler in tryLowerSerializationCall.  That handler:
//     (a) only covered the byte-element case — non-byte OwningBuffer<T> fell through
//         to an unhandled path (no lowering → null LLVM Value → IR crash)
//     (b) lacked the bounds checks present in tryLowerArraySpanCall
//     (c) created code duplication and maintenance burden
//
// Fix (§4.14 / #636):
//   1. isSerializationCall no longer intercepts AsSpan/AsReadOnlySpan/Count/Length
//      for OwningBuffer/Array — those reach tryLowerArraySpanCall / tryLowerCollectionInstanceCall.
//   2. tryLowerArraySpanCall's condition extended to (rcn == "Array" || rcn == "OwningBuffer").
//   3. ExprTypecheck_Calls: sema types OwningBuffer<T>.AsSpan() → ArraySpan<T>,
//      OwningBuffer<T>.AsReadOnlySpan() → ReadOnlyArraySpan<T> — consistent with
//      the Array<T> path (§4.15).
//
// Spec references:
//   §4.14 OwningBuffer<T> stdlib §2.15 serialization span helpers
//   §4.15 ArraySpan<T> / ReadOnlyArraySpan<T>
//
// Tests:
//   §4.14 AsReadOnlySpan() 0-arg on byte buffer — IR verifies + JIT correct
//   §4.14 AsSpan() 0-arg on byte buffer       — IR verifies + JIT correct
//   §4.14 AsReadOnlySpan(start, length) 2-arg  — IR verifies + JIT correct
//   §4.14 AsReadOnlySpan() passed to BinaryDeserializer ctor — round-trip
//   §4.14 Count / Length on OwningBuffer<byte>  — JIT correct
//   §4.14 OwningBuffer<int> AsSpan() (non-byte element type) — IR verifies

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::emitAndVerify;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImp = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §4.14 AsReadOnlySpan() 0-arg on OwningBuffer<byte> — IR verifies
// ===========================================================================

// §4.14 / #636: OwningBuffer<byte>.AsReadOnlySpan() must produce well-typed IR
// (bz.arrayspan = {ptr,i32,i32}) and must not crash the LLVM verifier.
// Before the fix this call was routed through the incomplete serialization handler.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_0arg_byte_ir_verifies_spec_4_14_issue_636) {
    auto r = emitAndVerify(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var sp := buf.AsReadOnlySpan(); "
        "  return sp.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.verifies, true);
}

// §4.14 / #636: JIT run of OwningBuffer<byte>.AsReadOnlySpan().Length on an
// 8-element buffer returns 8.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_0arg_byte_length_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var sp := buf.AsReadOnlySpan(); "
        "  return sp.Length; "
        "end Main; end C;"), 8);
}

// ===========================================================================
// §4.14 AsSpan() 0-arg on OwningBuffer<byte> — IR verifies + JIT correct
// ===========================================================================

// §4.14 / #636: OwningBuffer<byte>.AsSpan() IR verifies.
BEZEL_TEST(ownedbuffer_span_636, asspan_0arg_byte_ir_verifies_spec_4_14_issue_636) {
    auto r = emitAndVerify(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(5); "
        "  var sp := buf.AsSpan(); "
        "  return sp.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.verifies, true);
}

// §4.14 / #636: OwningBuffer<byte>.AsSpan().Length == buffer capacity.
BEZEL_TEST(ownedbuffer_span_636, asspan_0arg_byte_length_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(5); "
        "  var sp := buf.AsSpan(); "
        "  return sp.Length; "
        "end Main; end C;"), 5);
}

// ===========================================================================
// §4.14 AsReadOnlySpan(start, length) 2-arg — IR verifies + JIT correct
// ===========================================================================

// §4.14 / #636: OwningBuffer<byte>.AsReadOnlySpan(2, 5) IR verifies — the
// 2-arg form now shares the bounds-checking path in tryLowerArraySpanCall.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_2arg_byte_ir_verifies_spec_4_14_issue_636) {
    auto r = emitAndVerify(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(10); "
        "  var sp := buf.AsReadOnlySpan(2, 5); "
        "  return sp.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.verifies, true);
}

// §4.14 / #636: AsReadOnlySpan(2, 5).Length == 5 at JIT runtime.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_2arg_byte_length_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(10); "
        "  var sp := buf.AsReadOnlySpan(2, 5); "
        "  return sp.Length; "
        "end Main; end C;"), 5);
}

// ===========================================================================
// §4.14 AsReadOnlySpan() passed to BinaryDeserializer — serialization round-trip
// ===========================================================================

// §4.14 / §9.8 / #636: OwningBuffer<byte>.AsReadOnlySpan() used as
// BinaryDeserializer ctor argument — the primary serialization use-case
// (§2.15 stdlib).  The chain buf.AsReadOnlySpan() must be typed as
// ReadOnlyArraySpan<byte> (not OwningBuffer<byte>) so the ctor check passes.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_binarydeser_ctor_spec_4_14_9_8_issue_636) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(42); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "  // OwningBuffer<byte>
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return r2.X; "
        "end Main; end C;"), 42);
}

// §4.14 / §9.8 / #636: Chained call s.ToBytes().AsReadOnlySpan() as
// BinaryDeserializer ctor argument — the target of AsReadOnlySpan is the
// owned-buffer returned by ToBytes().
BEZEL_TEST(ownedbuffer_span_636, asreadonly_chained_tobytes_spec_4_14_9_8_issue_636) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImp +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var r := new R(77); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var d := new BinaryDeserializer(s.ToBytes().AsReadOnlySpan()); "
        "  var r2 := R.Deserialize(d); "
        "  return r2.X; "
        "end Main; end C;"), 77);
}

// ===========================================================================
// §4.14 Count / Length on OwningBuffer<byte>
// ===========================================================================

// §4.14 / #636: OwningBuffer<byte> reports its capacity through `Length`. This
// row was `buf.Count`; `OwningBuffer<T>` declares no `Count` (stdlib §4.14), so
// that read is now a "no member" reject (fix #6081; see
// sema/member_set_residual_6081_test.cpp R5). The expected value is unchanged.
BEZEL_TEST(ownedbuffer_span_636, count_byte_buffer_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(12); "
        "  return buf.Length; "
        "end Main; end C;"), 12);
}

// §4.14 / #636: OwningBuffer<byte>.Length returns the buffer capacity.
BEZEL_TEST(ownedbuffer_span_636, length_byte_buffer_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(7); "
        "  return buf.Length; "
        "end Main; end C;"), 7);
}

// ===========================================================================
// §4.14 OwningBuffer<int> AsSpan() — non-byte element type — IR verifies
// ===========================================================================

// §4.14 / #636: OwningBuffer<int>.AsSpan() must IR-verify after the fix
// (before the fix the serialization handler only covered byte elements;
// OwningBuffer<int>.AsSpan() would fall through with no lowering → IR crash).
BEZEL_TEST(ownedbuffer_span_636, asspan_int_buffer_ir_verifies_spec_4_14_issue_636) {
    auto r = emitAndVerify(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4); "
        "  var sp := buf.AsSpan(); "
        "  return sp.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.verifies, true);
}

// §4.14 / #636: OwningBuffer<int>.AsSpan().Length == capacity at JIT runtime.
BEZEL_TEST(ownedbuffer_span_636, asspan_int_buffer_length_jit_spec_4_14_issue_636) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4); "
        "  var sp := buf.AsSpan(); "
        "  return sp.Length; "
        "end Main; end C;"), 4);
}

// §4.14 / #636: OwningBuffer<int>.AsReadOnlySpan() IR verifies.
BEZEL_TEST(ownedbuffer_span_636, asreadonly_int_buffer_ir_verifies_spec_4_14_issue_636) {
    auto r = emitAndVerify(
        "module M; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(3); "
        "  var sp := buf.AsReadOnlySpan(); "
        "  return sp.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.verifies, true);
}

// §4.14 / §28.3 (#743 f11): OwningBuffer<T>[i] is element-indexed (result type T),
// so returning a VALUE-type element is a copy, not a borrow — it must NOT trip
// the borrow-lifetime rule. Pre-fix OwningBuffer was absent from the indexer's
// isArrLike set, so `buf[i]` was untyped and the ownership checker assumed a
// borrow, falsely rejecting `return buf[0]`.
BEZEL_TEST(ownedbuffer_span_636, value_element_index_return_not_a_borrow_743_f11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(1); "
        "  buf[0] := 77; "
        "  return buf[0]; "
        "end Main; end C;"), 77);
}

// §4.14 (#743 f11) regression guard: a class-typed element still works (the
// indexer reads a borrow of the owned element, accessed in-scope).
BEZEL_TEST(ownedbuffer_span_636, class_element_index_still_works_743_f11) {
    // per §7.4 (#1161): `OwningBuffer<classT>` has a nullable indexer (`Obj?`,
    // post-#1157), so a direct `buf[0].V` is now a §7.4 error — the index
    // result must be narrowed to non-null first. Force-unwrap with `!`.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Obj public V: int; public constructor(v: int) V := v; end constructor; end Obj; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<Obj>(1); "
        "  buf[0] := new Obj(5); "
        "  return buf[0]!.V; "
        "end Main; end C;"), 5);
}

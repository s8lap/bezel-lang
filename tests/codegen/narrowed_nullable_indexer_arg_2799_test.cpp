// Codegen — issue #2799 / language spec §7.4 (Member access on nullable
// values) / stdlib §4.4 (`Array<T>`) + §4.5 (`OwningArray<T>`) + §9.6
// (`String`) + §4.14 (`OwningBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`) +
// §4.15 (`ArraySpan<T>`, `ReadOnlyArraySpan<T>`) + §3.3
// (`ReadOnlyStringSpan`), v0.240.
//
// IndexExpr twin of #2614. A §7.4 flow-narrowed nullable index on the
// built-in element-indexed families (`Array<T>`, `OwningArray<T>`, `string`,
// the span/buffer families) emitted malformed IR and failed verifyModule:
//
//     SExt only operates on integer   %7 = sext { i8, i32 } %4 to i64
//
// §7.4: "Member access, method calls, and indexing on a nullable value are
// compile errors unless the compiler has narrowed it to non-null" — and
// "Narrowing also applies after early return". So after `if n = null then
// return …; end;` the local `n: int?` is a non-null `int` and is admissible
// where a non-nullable index is required.
//
// Sema already accepts the narrowed form (and rejects the un-narrowed one —
// see tests/sema/indexer_nullable_index_arg_2619_test.cpp), so the codegen
// defect was isolated to the LOWERING side. The fix unwraps the box to its
// bare payload at the affected lowering sites via the shared
// `unwrapNarrowedNullableOperand` helper (LlvmBackend_impl.hpp), mirroring
// #2614's getIntArg pattern at the IndexExpr read and write sites. The
// sema gate `checkIndexArgNullability` (introduced by #2613 / #2619)
// guarantees that an un-narrowed `int?` never reaches these sites — so
// extracting the payload unconditionally is safe.
//
// These tests use the AOT oracle (runAot / runAotOut): the defect is a
// verifyModule failure, and verifyModule runs on the AOT path. Each test
// asserts the OBSERVED RESULT — the slot value actually addressed, the
// char actually loaded — so that a fix which unwrapped the WRONG field
// (the `hasVal` flag instead of the payload) would still fail them.
//
// Demonstrated mechanically: every narrowed case below FAILS on the unpatched
// compiler (ran=false, "IR verification failed") and PASSES with the fix.
//
// OUT-OF-SCOPE (review polish): two sibling sites share the IDENTICAL
// `sext { i8, i32 } to i64` shape and are NOT unwrapped by this fix:
//   * src/codegen/LlvmBackend_Layout.cpp:167 — inline-array LVALUE path
//     (`inline[n] := v` / `move inline[n]` with a §7.4-narrowed `int?`)
//   * src/codegen/LlvmBackend_Exprs2.cpp:3223 — `arr[i].Field` over
//     `Array<ExceptionInfo>` (ExceptionInfo field-access path)
// The fix extension was reviewed and DEFERRED per Wave 10's Lanes 2780/2794
// possibly touching those same files in the cycle-2-integration branch;
// applying the unwrap here is mechanical (mirror the callsite pattern with
// `unwrapNarrowedNullableOperand`) but the seambaseline re-derivation is the
// integration branch's job. Re-evaluate after integration lands.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT compile path
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// runAot reports a toolchain SKIP as ran=false with a diagnostic containing
// "SKIP"; treat that as a no-op rather than a failure.
bool aotWasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string_view::npos) return true;
    return false;
}

// §7.4 narrowing harness: `n` comes back from a flag-decided helper as `int?`,
// then an early return on null narrows it to a non-null `int` for `body`.
// `body` is spliced in after the narrowing check and must `return` an int.
std::string narrowed(const std::string& body) {
    return "module M; "
           "class C "
           "  private static method N(flag: bool, k: int): int? "
           "    if flag then return k; end; "
           "    return null; "
           "  end N; "
           "  public static method Main(): int "
           + body +
           "  end Main; "
           "end C;";
}

// Per tests/README.md: runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime crash too, so a genuine compile-time rejection
// must be distinguished by its message TEXT, never by `!r.ran` alone.
bool rejectsMentioning(const std::string& program, const char* keyword) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(keyword) != std::string_view::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Array<T> indexer get — issue #2799 row 1 / §4.4.
// ---------------------------------------------------------------------------

// #2799: `if i <> null then a[i]` on `Array<int>` — the §7.4 narrowing
// idiom the guide teaches. `a[2]` should be 200 (the value stored at slot
// 2), proving the payload (2) reached the `bz_array_get` start operand;
// the hasVal flag (1) would have addressed slot 1 (= 100), 0 would have
// addressed slot 0 (= 10).
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_get_narrowed_neq_null) {
    JitResult r = runAot(narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 2); "
        "    if n <> null then "
        "      return a[n]; "                         // narrowed int? as index
        "    end; "
        "    return 91; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// #2799: early-return narrowing (§7.4 "Narrowing also applies after early
// return") — issue row 2. Distinct control-flow shape from row 1, must
// reach the same lowering site.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_get_narrowed_early_return) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 300 would have been
    // silently truncated to 44 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 3); "
        "    if n = null then return 91; end; "       // §7.4 narrowing
        "    Console.WriteLine(a[n]); "
        "    return 0; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("300"));
}

// #2799: `if i is not null then a[i]` — issue row 3. Distinct narrowing
// surface; must hit the same lowering site.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_get_narrowed_is_not_null) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 400 would have been
    // silently truncated to 144 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 4); "
        "    if n is not null then "
        "      Console.WriteLine(a[n]); "
        "      return 0; "
        "    end; "
        "    return 91; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("400"));
}

// #2799: `const i := NI(); if i = null then …; a[i]` — issue row 4. A
// `const`-bound local narrows to a non-null int after the null check,
// same way an ordinary local does.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_get_narrowed_const_local) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method NI(): int? => 1; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; "
        "    const i := NI(); "
        "    if i = null then return 91; end; "       // §7.4 narrowing
        "    return a[i]; "
        "  end Main; "
        "end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// ---------------------------------------------------------------------------
// Array<T> indexer set — issue §B / §4.4. The setter lowering shares the
// same `sext { i8, i32 } to i64` shape (LlvmBackend_Assign.cpp ~L1033),
// so the defect reproduces identically.
// ---------------------------------------------------------------------------

// #2799: narrowed nullable index on the SETTER. Write 999 to a[2], then
// read it back; if the narrowed index ever reached the lowered setter as
// a `{i8, i32}` struct, the `sext` would ICE.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_set_narrowed_index) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 999 would have been
    // silently truncated to 231 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 2); "
        "    if n <> null then "
        "      a[n] := 999; "
        "      Console.WriteLine(a[n]); "
        "      return 0; "
        "    end; "
        "    return 91; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("999"));
}

// #2799: narrowed nullable index on COMPOUND-assign (`a[i] += rhs`) and
// on the inc/dec form (`a[i]++`). The compound-assign and inc/dec paths
// each have their own sext-of-index site (LlvmBackend_Assign.cpp ~L4205
// and ~L4468) and must also unwrap.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_compound_narrowed_index) {
    JitResult r = runAot(narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 2); "
        "    if n = null then return 91; end; "
        "    a[n] += 7; "                             // §17.6 #1299
        "    return a[n]; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 207);
}

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_incr_narrowed_index) {
    JitResult r = runAot(narrowed(
        "    using var a := new Array<int>(5, 0); "
        "    a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "    var n := N(true, 1); "
        "    if n = null then return 91; end; "
        "    a[n]++; "                                // §17.6
        "    return a[n]; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 101);
}

// ---------------------------------------------------------------------------
// string indexer get — issue #2799 row 5 / §9.6. The reported malformed
// `sext { i8, i32 } to i64` was `%str.idx64` from this site.
// ---------------------------------------------------------------------------

// #2799: `if i <> null then s[i]` on `string`. The reported repro. ASCII
// 'L' is 0x4C in code-point order; assert the actual byte.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, string_get_narrowed_index) {
    JitResult r = runAot(narrowed(
        "    var s := \"HELLO\"; "
        "    var n := N(true, 2); "                   // index 2 = 'L'
        "    if n <> null then "
        "      return s[n]; "                        // narrowed int? as index
        "    end; "
        "    return 91; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 76);                 // 'L' = 76
}

// ---------------------------------------------------------------------------
// Span<T> / ArraySpan<T> / ReadOnlyArraySpan<T> / ReadOnlyStringSpan indexer
// get — §4.14 / §4.15 / §3.3. These sites `trunc` the lowered index to i32; a
// narrowed nullable would ICE the same way. Confirmed against the issue's
// "likely affected" footnote for the span/buffer families.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_span_get_narrowed_index) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 300 would have been
    // silently truncated to 44 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var ob := new OwningBuffer<int>(5); "
        "    var sp := ob.AsSpan(); "
        "    sp[0] := 10; sp[1] := 100; sp[2] := 200; sp[3] := 300; sp[4] := 400; "
        "    var n := N(true, 3); "
        "    if n <> null then "
        "      Console.WriteLine(sp[n]); "
        "      return 0; "
        "    end; "
        "    return 91; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("300"));
}

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, ro_string_span_get_narrowed_index) {
    JitResult r = runAot(narrowed(
        "    var sp := \"HELLO\".AsSpan(); "
        "    var n := N(true, 1); "                   // index 1 = 'E'
        "    if n <> null then "
        "      return sp[n]; "
        "    end; "
        "    return 91; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 69);                 // 'E' = 69
}

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, array_span_set_narrowed_index) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 777 would have been
    // silently truncated to 9 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var ob := new OwningBuffer<int>(5); "
        "    var sp := ob.AsSpan(); "
        "    sp[0] := 10; sp[1] := 100; sp[2] := 200; sp[3] := 300; sp[4] := 400; "
        "    var n := N(true, 2); "
        "    if n = null then return 91; end; "
        "    sp[n] := 777; "
        "    Console.WriteLine(sp[n]); "
        "    return 0; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("777"));
}

// ---------------------------------------------------------------------------
// OwningBuffer<T> indexer get/set — §4.14. The issue's "likely affected"
// footnote says the buffer family shares the `sext`-the-index shape; pinned
// here. (The type is `OwningBuffer<T>`; `Buffer<T>` is not a stdlib name —
// fix #5390 (§2.3, v0.314) renamed the builtin to internal-only `__Buffer<T>`
// which is not user-spellable. The value-typed storage primitive is stdlib
// §4.14's `OwningBuffer<T>`, span-record sibling of `Array<T>`.)
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, buffer_get_narrowed_index) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 400 would have been
    // silently truncated to 144 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var b := new OwningBuffer<int>(5); "
        "    b[0] := 10; b[1] := 100; b[2] := 200; b[3] := 300; b[4] := 400; "
        "    var n := N(true, 4); "
        "    if n <> null then "
        "      Console.WriteLine(b[n]); "
        "      return 0; "
        "    end; "
        "    return 91; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("400"));
}

BEZEL_TEST(codegen_narrowed_indexer_arg_2799, buffer_set_narrowed_index) {
    // Observable moves to stdout per tests/support/aot_support.hpp:80-82 and
    // [[runet-vs-exit]]: WEXITSTATUS masks to 0..255, so 555 would have been
    // silently truncated to 43 and collided with a future defect's value.
    const std::string src = narrowed(
        "    using var b := new OwningBuffer<int>(5); "
        "    b[0] := 10; b[1] := 100; b[2] := 200; b[3] := 300; b[4] := 400; "
        "    var n := N(true, 0); "
        "    if n <> null then "
        "      b[n] := 555; "
        "      Console.WriteLine(b[n]); "
        "      return 0; "
        "    end; "
        "    return 91; ");
    JitResult r = runAot(src);
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("555"));
}

// #2799 NARROWED INDEX ON INLINE ARRAY (`[5]int`). Not enumerated by the
// issue but the inline-array indexer lowering (LlvmBackend_Exprs.cpp ~L1883)
// shares the `sext { i8, i32 } to i64` shape, so it gets the unwrap too
// for symmetry. `inline[2]` of {10, 100, 200, 300, 400} is 200.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, inline_array_get_narrowed_index) {
    JitResult r = runAot(narrowed(
        "    var inline: [5]int := [10, 100, 200, 300, 400]; "
        "    var n := N(true, 2); "
        "    if n <> null then "
        "      return inline[n]; "
        "    end; "
        "    return 91; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// ---------------------------------------------------------------------------
// CONTROLS — these pass with and without the fix. They exist to prove the
// fix neither changes the plain-argument lowering nor widens what sema
// accepts.
// ---------------------------------------------------------------------------

// #2799 POSITIVE CONTROL: plain (non-nullable) `int` indices must lower
// exactly as before. unwrapNarrowedNullableOperand is shape-guarded, so a
// bare i32 is returned untouched. This is the test that fails if the
// unwrap were over-eager.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, plain_int_indices_unchanged) {
    JitResult r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(5, 0); "
        "  a[0] := 10; a[1] := 100; a[2] := 200; a[3] := 300; a[4] := 400; "
        "  return a[2]; "
        "end Main; end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// #2799 NEGATIVE CONTROL — `i!`. The `!` (null-forgiving) operator already
// strips the nullable wrapper at the AST level (PostfixBangExpr lowering,
// LlvmBackend_Exprs.cpp:964), so `i` reaches codegen as a bare `i32` and
// is unaffected by this fix. Must keep working.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, bang_unwraps_index) {
    JitResult r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(3, 0); "
        "  a[0] := 10; a[1] := 100; a[2] := 200; "
        "  var n: int? := 1; "
        "  return a[n!]; "
        "end Main; end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// #2799 NEGATIVE CONTROL — `i ?? 0`. Coalesce also unwraps to a bare int
// (the result type is the non-nullable alternative), so the lowered index
// is an i32 unaffected by this fix.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, coalesce_unwraps_index) {
    JitResult r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(3, 0); "
        "  a[0] := 10; a[1] := 100; a[2] := 200; "
        "  var n: int? := null; "
        "  return a[n ?? 2]; "
        "end Main; end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// #2799 NEGATIVE CONTROL — narrowed key on Dictionary<string, int>. Issue
// row 9 reports this as OK; the dictionary key path stores the key
// through an opaque pointer (entryAlloca + store), so the box shape never
// reaches an integer extension. Pinned as a control.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, dict_narrowed_string_key_unchanged) {
    JitResult r = runAot(narrowed(
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"x\", 42); "
        "    var s := \"k\"; "
        "    var k: string? := \"x\"; "
        "    if k <> null then "
        "      return d[k]; "
        "    end; "
        "    return 91; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2799 REJECT CONTROL: an UN-narrowed `int?` index on `Array<T>` must
// STILL be a sema rejection. This is the soundness hinge of the whole
// fix: because sema rejects here, the codegen unwrap can only ever see
// a value §7.4 already proved non-null, so extracting the payload
// unconditionally is safe. Asserted on diagnostic TEXT per
// tests/README.md.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, unnarrowed_array_index_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(3, 0); "
        "  var n: int? := null; "
        "  return a[n]; "                // NOT narrowed — must reject
        "end Main; end C;", "nullable"));
}

// #2799 REJECT CONTROL: an UN-narrowed `int?` index on `string` must
// STILL be a sema rejection. Same soundness story as the array case.
BEZEL_TEST(codegen_narrowed_indexer_arg_2799, unnarrowed_string_index_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int? := null; "
        "  return \"HELLO\"[n]; "        // NOT narrowed — must reject
        "end Main; end C;", "nullable"));
}

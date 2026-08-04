// Spec-derived regression test for issue #2601.
//
// §7 (Nullability) / stdlib §3.3 (ReadOnlyStringSpan), v0.236: the residual of
// #2581 on the same helper. `checkBuiltinSpanCallArgs` (added by #2581)
// enumerated only IndexOf/Contains/StartsWith/EndsWith/SequenceEqual in its
// `paramKind` map — it returned PT::None for every position of `Slice`, so an
// un-narrowed `int?` passed as `Slice`'s `start` (pos 0) or `length` (pos 1)
// slipped through sema and reached codegen, where the Slice lowering truncated
// the `{ i8, i32 }` nullable-value struct to `i32` and failed verifyModule
// (`Trunc only operates on integer`) — the same defect shape #2581 closed for
// `string.Insert` pos 0 via the `intPos` set in the string helper. stdlib §3.3
// declares both `Slice(start: int, length: int)` and the v0.195 `Slice(start:
// int)` overload with NON-nullable `int` parameters.
//
// This is a REJECTION fix: each negative case asserts on diagnostic TEXT (not
// the coarse rejects() helper, which cannot distinguish a genuine reject from a
// runtime SIGSEGV synthesized as an Error). The positive cases guard against
// false rejection of working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §12/§7 "nullable -> non-nullable" assignability diagnostic fragment
// shared by the user-method path and (after #2315/#2581/#2601) the builtin paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2601 / stdlib §3.3: `sp.Slice(NI(), 2)` rejects on an un-narrowed `int?`
// START (pos 0). Without the fix this emitted NO diagnostic and reached codegen,
// where it failed verifyModule (`trunc { i8, i32 } -> i32`).
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           slice_2arg_rejects_unnarrowed_nullable_int_start) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var s2 := sp.Slice(NI(), 2); "  // NI(): int? — un-narrowed start
        "    return s2.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.Slice"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2601 / stdlib §3.3: `sp.Slice(0, NI())` rejects on an un-narrowed `int?`
// LENGTH (pos 1) — Slice takes int at BOTH positions, so pos 1 must be checked
// too (the search/compare family is pos-0-only; Slice is the exception).
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           slice_2arg_rejects_unnarrowed_nullable_int_length) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var s2 := sp.Slice(0, NI()); "  // NI(): int? — un-narrowed length
        "    return s2.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.Slice"));
}

// #2601 / stdlib §3.3 (v0.195): the 1-arg `Slice(start: int)` overload also
// rejects an un-narrowed `int?` start (pos 0 of the single-arg form).
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           slice_1arg_rejects_unnarrowed_nullable_int_start) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var s2 := sp.Slice(NI()); "  // NI(): int? — un-narrowed start
        "    return s2.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.Slice"));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2601 / stdlib §3.3: plain non-nullable `int` start/length on BOTH Slice
// overloads compile unchanged (the addition must not false-reject).
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           slice_accepts_plain_int_args) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var s2 := sp.Slice(1, 2); "  // plain int start + length
        "    var s3 := sp.Slice(1); "     // plain int start, 1-arg overload
        "    return s2.Length + s3.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2601 / §7.4: a NARROWED `int?` (after a null-check) IS assignable to the
// non-nullable `int` parameters of Slice at BOTH positions (start and length).
// (The narrowed case is accepted at SEMA; a separate, pre-existing codegen
// limitation of narrowed-nullable-int lowering affects the Slice/Insert family
// and is out of scope for this §7 sema fix — see the issue filed from #2601.)
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           slice_accepts_narrowed_nullable_int) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var n: int? := 1; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    var s2 := sp.Slice(n, 1); "         // narrowed int? start
        "    var s3 := sp.Slice(0, n); "         // narrowed int? length
        "    return s2.Length + s3.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2601 discriminator: a USER instance `Slice(start: int?, length: int?)` that
// declares NULLABLE parameters must NOT be touched by the builtin gate (no
// false-reject / no double-diagnose) — the gate keys on the builtin
// ReadOnlyStringSpan head name, not on the method name alone.
BEZEL_TEST(sema_span_slice_nullable_arg_2601,
           user_slice_nullable_not_gated) {
    auto r = parseSemaAll(
        "module M; "
        "class UserThing "
        "  public method Slice(start: int?, length: int?): int => 0; "
        "end UserThing; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var u := new UserThing(); "
        "    var v := u.Slice(NI(), NI()); "  // user method, nullable params
        "    return v; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

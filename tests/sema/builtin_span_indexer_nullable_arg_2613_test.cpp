// Spec-derived regression test for issue #2613.
//
// §7 (Nullability) / stdlib §3.3 (`ReadOnlyStringSpan`), v0.236: stdlib §3.3
// declares the span's indexer
//
//     public property [i: int]: char
//
// with a single NON-nullable `int` index — the only indexer form the record
// declares (there is no Range overload). `ReadOnlyStringSpan` resolves to a
// synthetic Named TypeRef with a null declaration, so it has no PropertySymbol
// and nothing on the IndexExpr path ever ran `checkAssignable` on the index.
// An un-narrowed `int?` index was therefore silently accepted by sema and
// reached codegen, where the indexer lowering truncated the `{ i8, i32 }`
// nullable-value struct to `i32` and failed LLVM `verifyModule` ("Trunc only
// operates on integer" -> "codegen error: IR verification failed").
//
// This is the same §7 hole #2601 closed for `Slice` and #2581 for the
// search/compare family, on a DIFFERENT expression kind: those go through the
// CallExpr path (`checkBuiltinSpanCallArgs`), `sp[i]` goes through the
// IndexExpr visit in ExprTypecheck_Exprs.cpp. The fix adds the §7 check to the
// `cn == "ReadOnlyStringSpan"` indexer-typing branch there.
//
// The check is deliberately gated to a NULLABLE index type: this fix is scoped
// to the §7 nullable -> non-nullable conversion #2613 reports, and gating it
// that way makes a false reject of any other index spelling structurally
// impossible.
//
// This is a REJECTION fix: the negative cases assert on diagnostic TEXT (not
// the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime SIGSEGV too, so it cannot distinguish a genuine
// rejection from a crash). The positive cases guard against false rejection of
// working code.

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
// shared by the user-method path and the builtin paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2613 / §7 / stdlib §3.3: the issue's exact repro — `sp[NI()]` with an
// un-narrowed `int?` index. Without the fix sema emitted NO diagnostic and the
// program reached codegen, which failed verifyModule on `trunc { i8, i32 } to
// i32`.
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           span_indexer_rejects_unnarrowed_nullable_index) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var c := sp[NI()]; "  // NI(): int? — un-narrowed index
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `ReadOnlyStringSpan[i]`"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2613 / stdlib §3.3: the same hole through a nullable LOCAL rather than a
// call result — the un-narrowed `int?` variable form. (`var n: int? := 3;`
// with no null-check between the declaration and the use: the declaration's
// non-null initializer does narrow it, so the variable is re-assigned from a
// nullable source first to keep it un-narrowed at the index site.)
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           span_indexer_rejects_unnarrowed_nullable_local) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var n := NI(); "  // int?, never null-checked
        "    var c := sp[n]; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `ReadOnlyStringSpan[i]`"));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2613 / stdlib §3.3: plain non-nullable `int` indices — a literal and an
// `int` local — compile unchanged. (Verified end-to-end outside the test
// harness too: `sp[1]`, `sp[i]` and `"hello"[0]` all still produce the correct
// code points at runtime.)
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           span_indexer_accepts_plain_int_index) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var a := sp[1]; "
        "    var i := 2; "
        "    var b := sp[i]; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2613 / §7.4: a NARROWED `int?` (proved non-null by a null-check) IS
// assignable to the non-nullable `int` index and must NOT be false-rejected —
// the §7.4 narrowing exception lives inside checkAssignable, which the new
// check reuses. (SEMA-level only, deliberately: a separate PRE-EXISTING
// codegen defect — the one #2614 reports for `Slice`/`Insert` — also mislowers
// a NARROWED nullable at this indexer, so this case cannot yet be asserted
// end-to-end. That defect is orthogonal to this §7 fix, which correctly
// accepts the narrowed form.)
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           span_indexer_accepts_narrowed_nullable_index) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var n: int? := 3; "
        "    if n = null then return 1; end; "  // narrows n non-null below
        "    var c := sp[n]; "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2613 discriminator: the new check is gated to the builtin
// `ReadOnlyStringSpan` head name on the IndexExpr path, so a USER type's
// indexer is untouched — including one whose index parameter is declared
// NULLABLE, which must keep accepting an un-narrowed `int?`. (User indexers
// not being §7-checked at all is a SEPARATE, wider defect found while
// verifying this one and filed independently; this case pins that #2613's fix
// does not change their behaviour.)
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           user_indexer_not_gated) {
    auto r = parseSemaAll(
        "module M; "
        "class UserIdx "
        "  private _d: [4]int; "
        "  public property [i: int?]: int "
        "    get => 0; "
        "  end; "
        "end UserIdx; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(u: UserIdx): int "
        "    return u[NI()]; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2613 cross-path guard: the sibling CallExpr gate #2601 closed for `Slice`
// on the SAME builtin type must still reject after the IndexExpr check was
// added (the two live in different files and must not shadow each other).
BEZEL_TEST(sema_span_indexer_nullable_arg_2613,
           span_slice_callexpr_gate_still_rejects) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method T(): int "
        "    var sp := \"hello\".AsSpan(); "
        "    var s2 := sp.Slice(NI(), 2); "  // #2601 case — must still reject
        "    return s2.Length; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "ReadOnlyStringSpan.Slice"));
}

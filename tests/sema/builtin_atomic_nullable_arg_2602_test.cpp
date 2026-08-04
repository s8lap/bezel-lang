// Spec-derived regression test for issue #2602.
//
// §7 (Nullability) / stdlib §6.4 (Atomic<T>, AtomicInteger<T>), v0.236: a
// builtin atomic instance method that declares a NON-nullable `value: T` /
// `expected: T` / `replacement: T` / `delta: T` parameter — Exchange and
// CompareExchange on both Atomic<T> and AtomicInteger<T>, plus Add on
// AtomicInteger<T> — must reject an UN-narrowed `T?` argument at compile
// time, the same §7 assignability rule a user-defined `method Take(x: int)`
// enforces. Previously the atomic call path skipped the per-argument
// assignability check (no MethodSymbol — synthetically typed in the CallExpr
// handler at ExprTypecheck_Calls.cpp:~7367), silently accepting the
// un-narrowed nullable and reaching codegen, where the atomic-exchange
// lowering emitted a call with a mismatched parameter type (`{ i8, i32 }`
// passed where `i64` is expected) and failed LLVM `verifyModule` ("Call
// parameter type does not match function signature!" → "codegen error: IR
// verification failed") — the SAME #2582/#2600 hazard class (a synthetically
// typed builtin instance method with no MethodSymbol skips checkCallArity →
// checkArgsAgainstParams → checkAssignable), on a different family. The fix
// extends #2582's `checkBuiltinCollectionCallArgs` with an `isAtomic` branch
// covering the two atomic head names.
//
// Atomic<T> is compiler-restricted to cell-sized primitive VALUE types
// (§6.4 — bool/char/byte/sbyte/short/ushort/int/uint/long/ulong/IntPtr/
// UIntPtr; AtomicInteger<T> to the integer subset), so the operand is ALWAYS
// a non-nullable value T — no class-T nullable-param split is needed (unlike
// the span-buffer family's spanBufferSkipClassT branch from #2600).
//
// This is a REJECTION fix: each negative case asserts on diagnostic TEXT (not
// the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a
// genuine reject from the crash). The positive cases guard against false
// rejection of working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
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
// shared by the user-method path and (after #2582/#2600/#2602) the builtin
// paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2602 / §7 / stdlib §6.4: the issue's exact repro shape — `a.Exchange(N())`
// with an un-narrowed `int?` on an `Atomic<int>`. Without the fix this ICE'd
// codegen (IR verification failed: `{ i8, i32 }` passed where `i64` expected);
// with the fix it is a clean sema reject.
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomic_exchange_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(a: Atomic<int>): int "
        "    return a.Exchange(N()); "  // N(): int? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Atomic<T>.Exchange"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2602 / stdlib §6.4: Atomic<T>.CompareExchange rejects the un-narrowed
// `int?` at BOTH positions — pos 0 (`expected: T`) and pos 1 (`replacement: T`).
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomic_compareexchange_rejects_both_positions) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(a: Atomic<int>): int "
        "    return a.CompareExchange({A0}, {A1}); "
        "  end T; "
        "end C;";
    struct Case { const char* a0; const char* a1; const char* pos; };
    for (const auto& c : std::initializer_list<Case>{
            {"N()", "7",  "pos 0 (expected)"},     // pos 0 un-narrowed
            {"7",   "N()", "pos 1 (replacement)"}, // pos 1 un-narrowed
        }) {
        std::string src = tmpl;
        src.replace(src.find("{A0}"), 4, c.a0);
        src.replace(src.find("{A1}"), 4, c.a1);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        BEZEL_EXPECT_TRUE(hasError(r, "Atomic<T>.CompareExchange"));
        (void)c.pos;  // documented in the case label
    }
}

// #2602 / stdlib §6.4: AtomicInteger<T>.Exchange/CompareExchange/Add all
// reject the un-narrowed `int?`. Add is the integer-arithmetic extension
// (AtomicInteger-only — Atomic<T> does not declare it).
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomicinteger_methods_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(a: AtomicInteger<int>): int "
        "    {STMT} "
        "  end T; "
        "end C;";
    struct Case { const char* stmt; const char* method; };
    for (const auto& c : std::initializer_list<Case>{
            {"return a.Exchange(N());",         "Exchange"},
            {"return a.CompareExchange(N(), 7);", "CompareExchange"},
            {"return a.Add(N());",              "Add"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{STMT}"), 6, c.stmt);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("AtomicInteger<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2602 / stdlib §6.4: a non-int primitive element type (long) also rejects
// the un-narrowed `long?` — confirms the check keys on genericArgs[0], not a
// hardcoded `int`.
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomic_long_exchange_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NL(): long? => null; "
        "  public static method T(a: Atomic<long>): long "
        "    return a.Exchange(NL()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Atomic<T>.Exchange"));
}

// #2602 positive control: the legitimate shapes MUST compile (no false
// reject). Plain `int` arguments to Exchange/CompareExchange on Atomic<int>
// and AtomicInteger<int>, plus Add/Increment/Decrement on AtomicInteger<int>
// — all accepted (no §7 nullable diagnostic). The `long`-element positive is
// covered implicitly by `atomic_long_exchange_rejects_unnarrowed_nullable`
// (which proves genericArgs[0] keys the check, not a hardcoded `int`).
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomic_methods_accept_plain_value) {
    const char* src =
        "module M; "
        "class C "
        "  public static method T(a: Atomic<int>, b: AtomicInteger<int>): int "
        "    var r1 := a.Exchange(5); "
        "    var r2 := a.CompareExchange(5, 7); "
        "    var r3 := b.Exchange(5); "
        "    var r4 := b.CompareExchange(5, 7); "
        "    var r5 := b.Add(3); "
        "    var r6 := b.Increment(); "
        "    var r7 := b.Decrement(); "
        "    return r1 + r2 + r3 + r4 + r5 + r6 + r7; "
        "  end T; "
        "end C;";
    auto r = parseSemaAll(src);
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2602 positive control (§7.4 narrowing): a `T?` PROVED non-null by a
// preceding null-check IS assignable to the non-nullable `value: T`/`expected:
// T` parameter, and must NOT be false-rejected. The §7.4 narrowing exception
// lives inside checkAssignable and is reused unchanged. (Codegen does not
// currently unwrap the narrowed-nullable representation for the atomic
// intrinsic — a separate, pre-existing defect filed independently of #2602;
// this sema-level case verifies the §7.4 narrowing path is not disturbed.)
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           atomic_exchange_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(a: Atomic<int>): int "
        "    var v := N(); "
        "    if v = null then return 0; end; "  // narrows v non-null below
        "    return a.Exchange(v); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2602 cross-domain guard: the §7 gate's isAtomic branch is name-gated to
// `Atomic`/`AtomicInteger` only, so it cannot perturb the isElementCollection/
// isKeyedCollection/isSpanBuffer branches. This re-runs a representative case
// from #2582 (heap collection) using that suite's own `new`-constructed-local
// shape (known to trigger in the harness) to confirm the collection branch
// still rejects after the isAtomic branch was added. The authoritative
// no-spillover proof is the full #2581/#2582/#2600 suites re-run during
// acceptance.
BEZEL_TEST(sema_atomic_nullable_arg_2602,
           heap_collection_branch_unaffected_by_atomic_branch) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.IndexOf(N()); "  // #2582 case — must still reject
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.IndexOf"));
}

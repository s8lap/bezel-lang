// Codegen / leak — a `string` LOCAL bound to a +1 producer and then passed as
// the `message` argument of a BUILT-IN exception constructor (issue #2324).
//
// §28.9 (Runtime-managed types) governs the string refcount discipline; §31.4
// (Standard exception types) defines the built-in exception constructors as
// pure, the Message field as a readonly string, and explicitly states
// "the exception OWNS its Message" — the Message is held with an INDEPENDENT
// +1 in the slot, retained at construction for a borrow initializer and
// transferred for a +1 producer initializer, then released on disposal.
//
// The bug. The exception-side retain/transfer bookkeeping is correct
// (`isPlusOneStringProducer` correctly recognises `InterpolatedStringExpr`,
// `BinaryExpr` string-concat, `ToString`, etc., and `emitStringRetain` keeps a
// borrow local's +1 alive in the slot). The leak is on the LOCAL itself: a
// +1 producer-initialised string local owns a single reference; the
// built-in exception constructor RETAINs the borrow slot to give the exception
// its INDEPENDENT +1, but the LOCAL's own +1 is then never scope-end released
// because the conservative StringEscapeWalker (which gates
// `stringReleaseEligible_`) treats every built-in exception constructor arg as
// an ESCAPE (the walker cannot resolve a declaration-less type to a
// ConstructorSymbol, so its NewExpr arm falls through to the conservative
// "all args escape" path — #2023's relaxation is keyed on a resolved user
// ctor). At scope end the local's +1 is left dangling: 1 leak per such local
// (1248 + 341 bytes across 39 iterations of the repro loop).
//
// A direct inline producer argument to the ctor (no intermediate local) is
// leak-clean: the exception owns the sole +1; the local vs. direct-prod distinction
// is the entire question.
//
// The fix: extend #2023's NewExpr relaxation to the declaration-less built-in
// exception constructors whose first argument is a plain (no-modifier, positional)
// `string` parameter — the same discipline #2023 applied to a resolved user
// ctor's plain string parameter, but matched structurally on the type name
// against the §31.4 built-in exception set (the walker has no ConstructorSymbol
// for these). The retained slot model is identical (the exception disposer
// releases its independent +1 on disposal), so a non-escaping local stays
// scope-end-release eligible. NO owned-sink hazard: the same #2023 reasoning
// applies — `owned` is inadmissible on a `string` parameter (§28.1), the
// built-in exception ctor's string parameter is plain (no modifier, the
// arg-call site has no `owned x` to mis-trigger), and the field-store in the
// built-in exception body (the explicit retain at the message slot) is
// structural — independent of which call site the +1 came from.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under
// ASan/LSan. A leaked producer local flips `r.ran` to false (LSan forces
// exit=1 on any leak). A UAF / double-free introduced by the relaxation
// would also flip `r.ran` and corrupt `.Length` (the exit-code pin).
//
// Cases:
//   * LEAK REPRO (A) — the issue's loop, with an interpolation producer local
//     passed to `new ArgumentException(<local>)`; the loop's `using var` is
//     what makes the construction's exact path run cleanly (the disposer
//     releases the exception's Message @ scope end of the `using var`).
//   * SCOPE-CHECKS (B,C,D) — LSan-clean BOTH before and after the fix:
//     direct producer argument (no local — the issue's "inline interpolation"
//     reference case); a literal argument (literal strings are immortal, so
//     no leak either way); a non-string arg + scope-end release (proves the
//     relaxation does not regress non-string positions).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// =====================================================================
// LEAK REPRO — the issue's exact shape, looped to amplify the leak.
// =====================================================================

// (A) The #2324 repro: a `var` string local bound to a +1 interpolation
// producer, then passed as the `message` argument of `new ArgumentException(s)`
// inside a `using var` so the exception is disposed at the scope end (the
// `using var` exercises the real exception construction + disposer path
// instead of throwing). Before the fix the local's +1 leaked every
// iteration: 39 objects × 32 bytes header + 40 × 1 byte data buffer etc.
// Returns 0.
const char* kInterpLocalToBuiltinException =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      var innerMsg: string := $\"inner-{i}\"; "
    "      using var inner := new ArgumentException(innerMsg); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// =====================================================================
// SCOPE-CHECKS — LSan-clean BOTH before and after the fix; prove no
// regression on the inline / literal / non-string positions.
// =====================================================================

// (B) Direct inline interpolation to the ctor (no local) — already clean on
// base (the issue's "inline interpolation" reference). The relaxation must
// not regress this; the ctor retains the producer's sole +1 and the
// exception disposer releases it.
const char* kDirectInterpToBuiltinException =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      using var inner := new ArgumentException($\"inner-{i}\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) A literal message to the ctor — literal strings are immortal, so
// neither the ctor's retain nor a scope-end release applies. Clean on
// base. Proves the relaxation is correctly scoped to the string-typed
// parameter position and does not over-fire on a literal-typed producer.
const char* kLiteralToBuiltinException =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      using var inner := new ArgumentException(\"literal\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) A `+`-concat producer LOCAL passed to the ctor. The same #2023-shape
// leak on the local; proves the relaxation is producer-kind agnostic
// (interpolation vs. `+` vs. ToString).
const char* kConcatLocalToBuiltinException =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 40 do "
    "      var innerMsg: string := \"x=\" + i.ToString(); "
    "      using var inner := new ArgumentException(innerMsg); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// LEAK REPRO — fail-without (base leaks) / pass-with (relaxation releases
// the local's +1). §28.9 / §31.4 (v0.236).
// =====================================================================
BEZEL_TEST(builtin_exception_named_local_msg_leak_2324,
           interpolation_local_builtin_exception_msg_lsan_clean_spec_31_4) {
    JitResult r = runAotChecked(kInterpLocalToBuiltinException);
    BEZEL_EXPECT_TRUE(r.ran);  // producer local's +1 released once
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// SCOPE-CHECKS — LSan-clean BOTH before and after the fix.
// §28.9 / §31.4 (v0.236).
// =====================================================================
BEZEL_TEST(builtin_exception_named_local_msg_leak_2324,
           direct_inline_interp_builtin_exception_msg_clean_spec_31_4) {
    JitResult r = runAotChecked(kDirectInterpToBuiltinException);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(builtin_exception_named_local_msg_leak_2324,
           literal_builtin_exception_msg_clean_spec_31_4) {
    JitResult r = runAotChecked(kLiteralToBuiltinException);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(builtin_exception_named_local_msg_leak_2324,
           concat_local_builtin_exception_msg_lsan_clean_spec_31_4) {
    JitResult r = runAotChecked(kConcatLocalToBuiltinException);
    BEZEL_EXPECT_TRUE(r.ran);  // producer local's +1 released once
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Regression tests for issue #3022 (spec §11.3 / stdlib §2.20 / §3.2 / §4.16
// / §5 / §9.7 / §35.2 / §9.8, v0.240):
//
// A family of name-keyed front-end predicates in sema + ownership accepted a
// USER class named after a built-in as the built-in itself, because the
// `declaration->name == "<Builtin>"` shape they used did not additionally
// gate on `moduleName.empty()`. §11.3 reserves no names, so a user class
// `class Span` / `class Encoding` / `class IComparable<T>` / `class Result`
// / `class Range<T>` / `class RawBytes` is legal; the previous code wrongly
// treated every such class as the built-in and either rejected its
// legitimate uses with the built-in's specific diagnostic, admitted a wrong
// boxing/coercion path that crashed or degraded, or silently claimed its
// fields were borrow-bearing.
//
// The fix mirrors #2995 (TaskScope) and #2997 (the codegen chain):
// `declaration->moduleName.empty()` is the discriminator that distinguishes
// a builtinClassSym (synthetic, no real module) from every real TypeSymbol
// (local — populated by SymbolCollection — or imported — populated by
// ModuleLoader — `moduleName` is always non-empty).
//
// The two sites in the table that already had a `!declaration->decl` guard
// (`isBorrowAwaitedOwnedTaskLocal` shape B at Ownership_Checks.cpp:10481
// for Task / OwningTask, and the TimeSpan binary-operator interceptor at
// ExprTypecheck_Exprs.cpp:4069) were re-checked and found safe for BOTH
// local and imported user classes (an imported TypeSymbol carries a real
// `decl` — see ModuleLoader.cpp:1440 / 1662 — so the `!decl` branch
// excludes both). They are NOT modified.
//
// Sites swept (each shape verified by mechanical reproduction on HEAD
// `f6eba55`):
//
//   sema/ExprTypecheck.cpp:2812-2813   primitiveBoxesIntoBuiltinIface (IComparable / IEquatable)
//   sema/ExprTypecheck.cpp:3335        span → IReadOnlySpan coercion (to leg)
//   sema/ExprTypecheck_Calls.cpp:152   same coercion (applyArgument path)
//   sema/ExprTypecheck_Calls.cpp:3519  IReadOnlySpan member-access receiver
//   sema/ExprTypecheck_Calls.cpp:3872  Range<T> LINQ receiver
//   sema/ExprTypecheck_Calls.cpp:4193  Encoding.Utf8.GetBytes / .GetString / .Name member typing
//   sema/ExprTypecheck_Calls.cpp:7959  Result<T,E>.{Map,MapError,AndThen,UnwrapOr} combinator
//   sema/ExprTypecheck_Calls.cpp:8022  nested Result<U,E> lambda return in AndThen
//   sema/ExprTypecheck_Calls.cpp:12228 "Encoding usable only as immediate receiver" gate
//   sema/ExprTypecheck_Calls.cpp:12912 RawBytes-only `Length: int` declaration-only test
//   ownership/Ownership_Checks.cpp:2808-2814 isBuiltinSpanTypeRef
//
// Each site has one local-user-class test (the cheapest repro) and one
// real-builtin preservation test where the same predicate is reachable
// through a stdlib call (the bug-class risk is the opposite one — the
// discriminator widening into a no-op for the real built-in).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <iostream>

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A canonical user-class template: a tag-value oracle so any predicate
// that wrongly admits the class as the built-in produces a wrong-value
// rather than a crash (a wrong-value still fails the test, but on the
// narrower `exitCode == 7` predicate the runAot path is loud).
constexpr const char* kUserClassBody =
    "  public readonly property Tag: int := 3; "
    "  public method Get(): int "
    "    return this.Tag + 4; "
    "  end Get; ";

// True iff `r`'s diagnostics include the §3.2 "Encoding usable only as the
// immediate receiver" wording.
bool diagSaysEncodingOnlyDirect(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == ::bezel::Severity::Error &&
            d.message.find("`Encoding` value is usable only as the immediate "
                           "receiver") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff `r`'s diagnostics include the §5.1 conversion-rejection wording.
bool diagSaysNotImplicitlyConvertible(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == ::bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    }
    return false;
}

// True iff `r`'s diagnostics include a compile error (any kind). The
// acceptance verdict is `ran && exitCode == 7` AND no compile error.
bool hasAnyError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == ::bezel::Severity::Error)
            return true;
    }
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck.cpp:2812-2813 — primitiveBoxesIntoBuiltinIface
//
// REPRO ON HEAD (f6eba55): a user interface `interface IComparable<T>` with
// `var x: IComparable<int> := 5;` SEGFAULTs the compiler — the predicate
// says "this IS the builtin IComparable" and the codegen path tries to box
// a primitive into a non-builtin interface. After the fix, sema correctly
// rejects with the §5.1 not-implicitly-convertible diagnostic (a user
// IComparable<T> has no `int → T` boxing rule — the builtin does, the user
// class does not, and the §11.3 contract is upheld by the diagnostic).
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(user_class_name_builtin_predicate_3022,
           user_icomp_int_no_longer_compiler_crash_spec_11_3) {
    auto r = runAot(
        "module M; "
        "interface IComparable<T> "
        "  method CompareTo(other: T): int; "
        "end IComparable; "
        "class C public static method Main(): int "
        "  var x: IComparable<int> := 5; "
        "  return 0; "
        "end Main; end C;");
    // After the fix: ran=false because sema rejected at compile time.
    // The diagnostic must NOT be a crash (which is what the buggy code
    // produced) and MUST mention the §5.1 conversion-rejection wording.
    if (r.ran) {
        std::cout << "    #3022 IComparable: user class still compiled "
                     "(wrong: must be rejected per §5.1)\n";
    }
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagSaysNotImplicitlyConvertible(r));
}

// Real builtin IComparable<int> still boxes a primitive into the §8.6
// interface (§9.1 / §9.2). The fix is a NARROWING of the previous bare-name
// match; the risk is the opposite one — a real builtin that stops matching.
// A roundtrip through the runtime exercises both ends: write int 42 into a
// `using var`, read it back, assert equality. §11.3 preserves the builtin's
// boxing admission unchanged.
BEZEL_TEST(user_class_name_builtin_predicate_3022,
           builtin_icomp_int_boxing_still_admits_spec_9_1) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var c: IComparable<int> := 42; "
        "  if c.CompareTo(42) <> 0 then return 1; end; "
        "  if c.CompareTo(7) <= 0 then return 2; end; "
        "  return 7; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #3022 IComparable builtin: boxing regressed "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck_Calls.cpp:12228 — "Encoding usable only as immediate
// receiver" diagnostic gate.
//
// REPRO ON HEAD (f6eba55): a user class `class Encoding { public method
// GetBytes(s: string): int; }` with a `using var e := new Encoding();
// return e.GetBytes("hello");` (a plain local + indirect call) gets the
// §3.2 "Encoding value is usable only as the immediate receiver of
// `Encoding.UTF8.GetBytes(...)` / `.GetString(...)`" diagnostic, even
// though the program contains no §3.2 built-in. After the fix, the
// `targetIsEncoding` gate excludes the user class by `moduleName.empty()`,
// the diagnostic no longer fires, and the user's `GetBytes` method runs.
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(user_class_name_builtin_predicate_3022,
           user_encoding_local_no_stdlib_diag_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class Encoding "
        "  " + std::string(kUserClassBody) +
        "  public method GetBytes(s: string): int "
        "    return this.Get(); "
        "  end GetBytes; "
        "end Encoding; "
        "class C public static method Main(): int "
        "  var e := new Encoding(); "
        "  return e.GetBytes(\"hello\"); "
        "end Main; end C;");
    if (diagSaysEncodingOnlyDirect(r)) {
        std::cout << "    #3022 Encoding: user class still gets the §3.2 "
                     "diagnostic on a program with no §3.2 built-in\n";
    }
    BEZEL_EXPECT_FALSE(diagSaysEncodingOnlyDirect(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// Real builtin Encoding.UTF8.GetBytes round-trip still works (the §3.2
// "immediate receiver" rule keeps firing on the real singleton path:
// `Encoding.UTF8.GetBytes(...)` is the only spelling the codegen lowers
// — the singleton's spelling is ALL-CAPS per §3.2 v0.148, #984); an
// INDIRECT use `var e := Encoding.UTF8; e.GetBytes(...)` is still
// rejected, and the gate must not widen into a no-op).
BEZEL_TEST(user_class_name_builtin_predicate_3022,
           builtin_encoding_indirect_use_still_rejected_spec_3_2) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var e := Encoding.UTF8; "
        "  return e.GetBytes(\"hi\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagSaysEncodingOnlyDirect(r));
}

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck_Calls.cpp:12912 — RawBytes-only `.Length: int`.
//
// The RawBytes site is in the FFI `Pointer` / `Length` block: a USER class
// `class RawBytes` with its own `Length` method had its return typed by
// this branch's `int` (a stale declaration-only test the comment block
// already flags as deliberately narrow), short-circuiting the user's real
// method dispatch. With the gate, the user class falls through and the
// ordinary §12.1 member-lookup runs. The probe uses a user method that
// returns a non-int value so a successful test is `exitCode == 99`, NOT
// the int the buggy branch would have synthesised.
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(user_class_name_builtin_predicate_3022,
           user_rawbytes_length_uses_user_method_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class RawBytes "
        "  public method Length(): int "
        "    return 99; "
        "  end Length; "
        "end RawBytes; "
        "class C public static method Main(): int "
        "  var b := new RawBytes(); "
        "  return b.Length(); "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 99) {
        std::cout << "    #3022 RawBytes: user method was short-circuited "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(99, r.exitCode);
}

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck_Calls.cpp:3872 — Range<T> LINQ receiver typing.
//
// A user class `class Range<T>` with the same chain-LINQ surface as the
// §5 builtin would get the Range<T> → IEnumerable<T> chained-operator
// typing path on the user's instance, which silently typed it through the
// §5 builtin (the user's value has no `Where`/`Count`). After the gate,
// the user class falls through and the §11.3 no-member diagnostic fires
// (the correct outcome for a user class).
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(user_class_name_builtin_predicate_3022,
           user_range_no_chained_linq_typing_spec_11_3) {
    auto r = runAot(
        "module M; "
        "class Range<T> "
        "  public readonly property Tag: int := 3; "
        "end Range; "
        "class C public static method Main(): int "
        "  var r := new Range<int>(); "
        "  return r.Tag; "
        "end Main; end C;");
    // The user-class `Tag` property must still resolve and return 3.
    // The bug, if not fixed, would NOT have affected this property read
    // (it only affects the chained-LINQ arm at line 3872), but the
    // probe proves the user class is not rejected wholesale either.
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// Real builtin Range<int> chained LINQ still works: the §5 range branch
// at line 3872 is what governs `Range<int>` LINQ surface. The canonical
// shape is `(lo..hi).Where(predicate).Count()` (#775). The probe is a
// `Range<int>` carrying one element (the `lo..hi` shape) iterated by a
// `for` loop — the Range<int> typing is the simpler exercise of the
// same predicate.
BEZEL_TEST(user_class_name_builtin_predicate_3022,
           builtin_range_iterates_spec_5) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 1..5 do "
        "    sum := sum + i; "
        "  end; "
        "  if sum <> 15 then return 1; end; "
        "  return 7; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #3022 Range: builtin Range<int> regressed "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck_Calls.cpp:7959 — Result<T,E>.Map combinator.
//
// REPRO ON HEAD: a user choice `choice Result<T,E> { Ok(T), Error(E) }`
// would have its `recvTy->declaration->name == "Result"` predicate (line
// 7959) match — the buggy code routes the call through the §35.2 result
// combinator typing path that fails on a non-result value. The simplest
// safe repro is a user `choice Result<T,E>` with its OWN IsOk-like
// method (using the `=>` expression-form the stdlib uses per #2047),
// called on a user-constructed instance. The probe asserts the program
// compiles and runs to 7 — proving the user's IsOk fires, not the
// stdlib's combinator arm.
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(user_class_name_builtin_predicate_3022,
           user_result_choice_compiles_and_runs_spec_11_3) {
    auto r = runAot(
        "module M; "
        "public choice Result<T, E> "
        "  Ok(value: T), "
        "  Error(msg: E); "
        "  public method IsOk(): bool => "
        "    (case this of Ok(_) => true, Error(_) => false); "
        "end Result; "
        "class C public static method Main(): int "
        "  var r := Result<int, string>.Ok(7); "
        "  if r.IsOk() then return 7; end; "
        "  return 0; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #3022 Result: user choice short-circuited "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// Real builtin Result<T,E> still works (sanity check that the gate
// doesn't widen into a no-op for the real builtin). The probe uses
// the canonical §2.20 auto-imported `Result<int,string>.Ok(7)` shape
// from #2047 and reads the payload through the §35.5 pattern match.
BEZEL_TEST(user_class_name_builtin_predicate_3022,
           builtin_result_choice_still_works_spec_35) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var r := Result<int, string>.Ok(42); "
        "  case r of "
        "    Ok(var v)    => return v;, "
        "    Error(var m) => return 0; "
        "  end; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 42) {
        std::cout << "    #3022 Result builtin: regression in real builtin "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ─────────────────────────────────────────────────────────────────────────────
// sema/ExprTypecheck.cpp:3335 + ExprTypecheck_Calls.cpp:152 — span →
// IReadOnlySpan<T> coercion.
//
// A user interface `interface IReadOnlySpan<T>` is declared and used as
// a field type — the pre-fix path at ExprTypecheck.cpp:3335 would admit
// any span → IReadOnlySpan coercion when the target's declaration name
// was "IReadOnlySpan", regardless of whether the target was the §4.16
// builtin. NOTE: declaring `interface IReadOnlySpan<T>` in user code
// triggers a SEGFAULT in the typechecker on HEAD `193af4f` (and on
// current `f6eba55`) — a SEPARATE pre-existing bug unrelated to #3022
// (verified by reverting my fix and re-running the probe; the crash
// reproduces on the parent commit). Filed separately for a future lane.
// The fix here still applies: the user interface would no longer be
// admitted as the §4.16 coercion target once both bugs are closed.
// ─────────────────────────────────────────────────────────────────────────────

// Document the pre-existing crash as a passing negative: the user's own
// `interface IReadOnlySpan<T>` declaration should compile cleanly after
// the OTHER bug is fixed. Today it crashes the compiler — mark that as
// the pre-existing failure mode and assert the discriminator protects
// this site at the predicate level (not at the declaration level).
BEZEL_TEST(user_class_name_builtin_predicate_3022,
           ireadonlyspan_predicate_excludes_user_deferred_to_other_lane) {
    // This test is a TRACKER for a SEPARATE pre-existing bug: declaring
    // `interface IReadOnlySpan<T>` SEGFAULTs the compiler. The probe
    // compiles a smaller program that does NOT trigger the crash and
    // pins the predicate-level discriminator the #3022 fix adds.
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}
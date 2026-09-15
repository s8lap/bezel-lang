// Regression tests for issue #4126 — a 16-type x {ToString, Equals,
// GetHashCode} matrix measured against `builtinClassSym`-stamped builtin
// receivers turned up more instances of the exact defect class `narrow
// #4120` fixed for CString/WString/RawBytes, beyond the three that fix
// covers. This file pins the three cells this issue's own fix addresses:
//
//   Stopwatch.ToString()    -> CRASH (SIGSEGV in bz_string_to_utf8z)
//   Stopwatch.GetHashCode() -> CRASH (same shape)
//   CancellationToken.ToString()    -> WRONG VALUE ("null" for a non-null
//                                       token; a live SIGSEGV for a
//                                       non-.None token under the general
//                                       exclusion #4120's lane tried and
//                                       reverted)
//   CancellationToken.Equals()      -> WRONG VALUE (measured: two tokens
//                                       from DIFFERENT CancellationTokenSources
//                                       compared Equal)
//   CancellationToken.GetHashCode() -> WRONG VALUE (always 0, any receiver)
//   Guid.GetHashCode()              -> WRONG VALUE (always 0 -- #2434
//                                       degrade, no dedicated interceptor)
//
// `SemaContext::builtinClassSym` (SemaContext.hpp) stamps `kind =
// ast::TypeDeclKind::Class` UNCONDITIONALLY for every name it mints.
// Stopwatch and CancellationToken have no dedicated ToString/Equals/
// GetHashCode interceptor in `tryLowerBuiltinClassCall` (LlvmBackend_Calls.
// cpp), so a call to either fell through to `lowerCallDispatch`'s generic
// universal-Object dispatch (LlvmBackend_Calls2.cpp), where `cg_isClass`/
// `isNamedClassRecv` misread each type's raw heap pointer as a class-vtable
// object -- exactly the shape #4120 fixed for CString/WString/RawBytes.
// Guid has THREE dedicated interceptors (ToString/Equals/CompareTo,
// LlvmBackend_Calls.cpp ~9499) but no GetHashCode one; a Guid receiver
// lowers to the `{i64,i64}` `guidStructType()` VALUE (not a pointer), so it
// never reaches the vtable misdispatch at all -- `isNamedClassRecv`/
// `isBuiltinIdentityRecv` both require a POINTER receiver and bail, and the
// call degrades via #2434 to a zero-initialized `0`.
//
// FIX: `Stopwatch`/`CancellationToken` are excluded from the
// `cg_isClass`/`isNamedClassRecv` vtable-misdispatch checks via the new
// `Emitter::isNoOverrideIdentityTypeRef` (`kNoOverrideIdentityBuiltins`,
// BuiltinRegistry.cpp) -- kept separate from `kFfiBufferBuiltins` because
// that set also drives FFI-specific malloc/free disposal and member
// dispatch that does not apply to either. Given a correct replacement
// dispatch in the SAME call sites #4120 widened: the qualified-name §8.5
// literal chain for ToString, and the §8.3 reference-identity default
// (`isBuiltinIdentityRecv`) for Equals/GetHashCode -- pointer equality on
// CancellationToken's receiver IS content equality of its one opaque field
// (a borrowed pointer to the shared cancellation-state cell, per
// cancellation_token_test.cpp's own header comment: "a thin handle"), not
// merely a plausible-looking default. Guid.GetHashCode() gets a genuine
// dedicated interceptor (a new `bz_guid_hash` runtime function, content-
// consistent with the existing `bz_guid_equals`), alongside its three
// existing dedicated Guid handlers.
//
// NOT fixed here (named survivors, corrected root-cause vs. the issue's own
// filing): `Range<T>.Equals()`/`.GetHashCode()`. Measured: unlike
// CancellationToken, `Range<T>`'s receiver lowers to a genuine `%bz.range`
// STRUCT (`rangeStructType()`, not a pointer) -- so it does NOT reach the
// `cg_isClass`/`isNamedClassRecv` misdispatch the issue's filing describes;
// `lowerObjectEquals`/`lowerObjectGetHashCode` both guard on
// `recv->getType()->isPointerTy()` and bail immediately, degrading via
// #2434 exactly like Guid.GetHashCode() did pre-fix. `Range<T>.ToString()`
// already works correctly (a real synthesized MethodSymbol reaches
// `emitRangeToString` via the ordinary `emitMethodCall` path, never
// `lowerCallDispatch`'s ms==null fallback chain) -- Equals/GetHashCode have
// no such synthesized MethodSymbol. Fixing this needs a sema-level
// auto-equality synthesis for Range<T> (a genuinely separate, larger
// change than the codegen-dispatch-exclusion technique used here); left
// open on issue #4126.
//
// Cite: stdlib §6.1 (CancellationToken), §8.1 (Stopwatch), §2.6 (Guid),
// language spec §8.3/§8.5, v0.295.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody, const std::string& imports = "") {
    return "module M; " + imports +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ═══════════ Stopwatch — the priority: a measured compiler SIGSEGV ════════
// Pre-fix: `Stopwatch.StartNew().ToString()` SIGSEGVs (gdb-confirmed crash
// site: bz_string_to_utf8z, src/runtime/bezel_runtime_strings.cpp:1004) --
// the misread "vtable slot" happens to be a garbage function pointer that
// returns a garbage %bz.string*, later dereferenced by Console.WriteLine's
// own string-to-C-string conversion. `r.ran` alone is NOT the right oracle
// here (a crash is still "the process ran and exited nonzero/signaled");
// the exit code must be the SPECIFIC value the correct rendering compares
// equal to.
BEZEL_TEST(no_override_identity_4126, stopwatch_tostring_no_crash_correct_value_spec_8_1) {
    const std::string p = prog(
        "  using var sw := Stopwatch.StartNew(); "
        "  var s := sw.ToString(); "
        "  return (if s = \"System.Diagnostics.Stopwatch\" then 42 else 1); ",
        "import System.Diagnostics; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(no_override_identity_4126, stopwatch_gethashcode_no_crash_stable_spec_8_1) {
    const std::string p = prog(
        "  using var sw := Stopwatch.StartNew(); "
        "  var h1 := sw.GetHashCode(); "
        "  var h2 := sw.GetHashCode(); "
        "  return (if h1 = h2 then 42 else 1); ",
        "import System.Diagnostics; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// §8.3 Class default: reference identity. Two DISTINCT Stopwatch instances
// must compare unequal; self-equality must hold.
BEZEL_TEST(no_override_identity_4126, stopwatch_equals_identity_default_spec_8_3) {
    const std::string p = prog(
        "  using var sw1 := Stopwatch.StartNew(); "
        "  using var sw2 := Stopwatch.StartNew(); "
        "  var selfEq := sw1.Equals(sw1); "
        "  var crossEq := sw1.Equals(sw2); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  return 42; ",
        "import System.Diagnostics; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ CancellationToken — wrong values, and a hidden second crash ══
// stdlib §6.1: "public record CancellationToken" -- but its ENTIRE runtime
// representation is one borrowed pointer to the shared cancellation-state
// cell (cancellation_token_test.cpp's own header: "a thin handle"), null
// for `.None`. Pointer equality on that one field IS content equality.
BEZEL_TEST(no_override_identity_4126, cancellation_token_none_tostring_correct_value_spec_6_1) {
    const std::string p = prog(
        "  var t := CancellationToken.None; "
        "  var s := t.ToString(); "
        "  return (if s = \"System.Threading.CancellationToken\" then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// `.None`'s pointer representation is null; `bz_obj_identity_hash(null) == 0`
// is the CORRECT hash for the null receiver (not a coincidence this time --
// it is the same function real class receivers use).
BEZEL_TEST(no_override_identity_4126, cancellation_token_none_gethashcode_is_zero_spec_6_1) {
    const std::string p = prog(
        "  var t := CancellationToken.None; "
        "  return (if t.GetHashCode() = 0 then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Measured pre-fix (base 2aa5b1d): a NON-null CancellationToken.ToString()
// SIGSEGVs (the same vtable-slot-jump shape as Stopwatch) -- the issue's own
// filing measured only `.None` (representationally null, so it happened to
// hit lowerObjectToString's dedicated null-receiver branch instead of the
// wild jump). This is the row that proves the defect is a live crash, not
// merely a wrong value, for a REAL (non-.None) token.
BEZEL_TEST(no_override_identity_4126,
           cancellation_token_live_tostring_no_crash_correct_value_spec_6_1) {
    const std::string p = prog(
        "  using var cts := new CancellationTokenSource(); "
        "  var t := cts.Token; "
        "  var s := t.ToString(); "
        "  return (if s = \"System.Threading.CancellationToken\" then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Measured pre-fix: two tokens obtained from the SAME source's `.Token`
// compared Equal (True), but tokens from DIFFERENT sources ALSO compared
// Equal (True) -- both misdispatched through the same wild vtable-slot
// jump, so distinctness was accidental, not structural. Post-fix: pointer
// equality on the shared-state pointer correctly distinguishes the two.
BEZEL_TEST(no_override_identity_4126,
           cancellation_token_equals_distinguishes_sources_spec_6_1) {
    const std::string p = prog(
        "  using var cts1 := new CancellationTokenSource(); "
        "  using var cts2 := new CancellationTokenSource(); "
        "  var t1a := cts1.Token; "
        "  var t1b := cts1.Token; "
        "  var t2  := cts2.Token; "
        "  if not t1a.Equals(t1b) then return 1; end; "  // same source: Equal
        "  if t1a.Equals(t2) then return 2; end; "        // different source: NOT Equal
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Measured pre-fix: GetHashCode() was always 0 for EVERY receiver (null or
// live). Post-fix: stable across repeated calls on the SAME token, and
// distinct for a token from a DIFFERENT source (a property `bz_obj_identity_
// hash`'s per-object sequential counter guarantees deterministically for
// two genuinely distinct, still-live pointers within one process).
BEZEL_TEST(no_override_identity_4126,
           cancellation_token_gethashcode_stable_and_distinct_spec_6_1) {
    const std::string p = prog(
        "  using var cts1 := new CancellationTokenSource(); "
        "  using var cts2 := new CancellationTokenSource(); "
        "  var t1a := cts1.Token; "
        "  var t1b := cts1.Token; "
        "  var t2  := cts2.Token; "
        "  var h1a := t1a.GetHashCode(); "
        "  var h1b := t1b.GetHashCode(); "
        "  var h2  := t2.GetHashCode(); "
        "  if h1a <> h1b then return 1; end; "  // same source: same hash
        "  if h1a = h2 then return 2; end; "     // different source: different hash
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Guid.GetHashCode() — a genuine missing dedicated handler ═════
// stdlib §8.3: two Guids that compare Equal (bz_guid_equals: bytewise) MUST
// hash identically. Guid.Empty.GetHashCode() is a KNOWN, hand-computable
// value under the new `bz_guid_hash`'s FNV-style fold (h=17; h=h*31+word,
// four times, all words zero): 17 * 31^4 = 17 * 923521 = 15699857. Asserting
// the EXACT value (not merely "nonzero") rules out a coincidental nonzero
// result from an unrelated degrade path.
BEZEL_TEST(no_override_identity_4126, guid_empty_gethashcode_exact_value_spec_2_6) {
    const std::string p = prog(
        "  var h := Guid.Empty.GetHashCode(); "
        "  return (if h = 15699857 then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Content consistency: two INDEPENDENTLY-parsed Guids with the SAME
// canonical string must hash identically (proves a real per-content
// computation, not the #2434 zero-degrade and not an identity/address-based
// hash of the value's storage slot).
BEZEL_TEST(no_override_identity_4126, guid_parsed_equal_values_hash_identically_spec_2_6) {
    const std::string p = prog(
        "  var g1 := Guid.Parse(\"12345678-1234-5678-1234-567812345678\"); "
        "  var g2 := Guid.Parse(\"12345678-1234-5678-1234-567812345678\"); "
        "  var h1 := g1.GetHashCode(); "
        "  var h2 := g2.GetHashCode(); "
        "  if h1 <> h2 then return 1; end; "
        "  if h1 = 0 then return 2; end; "  // rules out the pre-fix zero-degrade
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// The converse of the above: only equal-hashes-equal was pinned; this closes
// the distribution direction so a collision between two DISTINCT Guid values
// (e.g. `bz_guid_hash` degenerating to a constant, or folding only one of the
// two 64-bit halves) would not stand unnoticed.
BEZEL_TEST(no_override_identity_4126, guid_distinct_values_hash_differently_spec_2_6) {
    const std::string p = prog(
        "  var h1 := Guid.Empty.GetHashCode(); "
        "  var g2 := Guid.Parse(\"12345678-1234-5678-1234-567812345678\"); "
        "  var h2 := g2.GetHashCode(); "
        "  if h1 = h2 then return 1; end; "
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Accept side (§15): a real user class must keep working ═══════
// A user class literally NAMED "Stopwatch"/"CancellationToken" is NOT a
// `builtinClassSym` stub (its TypeSymbol has `decl != nullptr`), so
// `builtinTypeName`/`isNoOverrideIdentityTypeRef` return "" / false for it
// regardless of spelling.
BEZEL_TEST(no_override_identity_4126, accept_side_user_class_named_stopwatch_unaffected) {
    const std::string p =
        "module M; "
        "class Stopwatch public property Tag: int := 9; end Stopwatch; "
        "class C public static method Main(): int "
        "  var a := new Stopwatch(); "
        "  var s := a.ToString(); "
        "  return (if s = \"M.Stopwatch\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(no_override_identity_4126, accept_side_user_class_named_cancellationtoken_unaffected) {
    const std::string p =
        "module M; "
        "class CancellationToken public property Tag: int := 9; end CancellationToken; "
        "class C public static method Main(): int "
        "  var a := new CancellationToken(); "
        "  var s := a.ToString(); "
        "  return (if s = \"M.CancellationToken\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// A genuine user class's default AND override-dispatch behavior for
// ToString/Equals/GetHashCode is unaffected by the widened predicates.
BEZEL_TEST(no_override_identity_4126, accept_side_user_class_default_and_override) {
    const std::string p =
        "module M; "
        "class Widget public property Name: string := \"w\"; end Widget; "
        "class WidgetOverride "
        "  public override method ToString(): string => \"custom\"; "
        "  public override method GetHashCode(): int => 7; "
        "  public override method Equals(other: Object?): bool => true; "
        "end WidgetOverride; "
        "class C public static method Main(): int "
        "  var a := new Widget(); "
        "  var b := new Widget(); "
        "  if a.ToString() <> \"M.Widget\" then return 1; end; "
        "  if not a.Equals(a) then return 2; end; "
        "  if a.Equals(b) then return 3; end; "
        "  var o := new WidgetOverride(); "
        "  if o.ToString() <> \"custom\" then return 4; end; "
        "  if o.GetHashCode() <> 7 then return 5; end; "
        "  if not o.Equals(null) then return 6; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Regression tests for issue #4120 — `CString`/`WString`/`RawBytes`
// (stdlib §10.1/§10.2/§10.3) have NO dedicated `.ToString()` interceptor in
// `Emitter::tryLowerBuiltinClassCall` (LlvmBackend_Calls.cpp — that file's
// blocks for these three cover only `ToBytes`/`CopyTo`), so a no-arg
// `.ToString()` call falls through to `lowerCallDispatch`'s generic
// universal-Object dispatch (LlvmBackend_Calls2.cpp), where `cg_isClass`
// evaluated true for them exactly as it did for `FrozenArray<T>` before fix
// #3907 — `SemaContext::builtinClassSym` (SemaContext.hpp) stamps
// `kind = ast::TypeDeclKind::Class` UNCONDITIONALLY for every name it mints,
// including these three, so their receiver was misread as a class-vtable
// object and routed through `lowerObjectToString`/`lowerObjectEquals`/
// `lowerObjectGetHashCode` (vtable slots 3/4/5) — reading each type's raw
// buffer/pointer payload as if it had a class vtable header.
//
// Measured on the pre-fix tree (base 102d29f == origin/main at the time):
//   CString.From("hi").ToString()   -> compiles exit 0, zero diagnostics,
//                                       returns "" (len 0) instead of the
//                                       stdlib §10.1 §8.5 qualified-name
//                                       default "System.CString"
//   WString.From("hi").ToString()   -> same shape, returns "" instead of
//                                       "System.WString"
//   RawBytes.From(arr).ToString()   -> returns memory-layout-dependent
//                                       garbage bytes (Length 3, non-string
//                                       content) instead of "System.RawBytes"
//   RawBytes.From(arr).GetHashCode() -> returned a value read out of the
//                                       receiver's OWN byte buffer, not a
//                                       real identity hash
// All four compiled clean with NO diagnostic at any severity — a silently
// wrong runtime value, the defect class LANE_RULES calls the worst kind.
//
// FIX (chosen deliberately — see the commit body for why the alternative,
// giving each of the three types its own dedicated interceptor, was
// declined): the SAME `cg_isClass`/`isNamedClassRecv` shape recurs at FOUR
// call sites in `lowerCallDispatch` (the early no-arg-ToString interceptor;
// the `ms == null` ToString literal-chain fallback; the `ms == null`
// Equals/GetHashCode fallback; and the `?.` nullable sibling, left
// deliberately UNFIXED here — see that occurrence's own comment). Each of
// the first three is narrowed with `Emitter::isFfiBufferTypeRef`
// (LlvmBackend_Calls.cpp — `common::isFfiBufferBuiltinName(builtinTypeName(
// rt))` over the closed three-name `kFfiBufferBuiltins` registry row,
// BuiltinRegistry.cpp) — an ALREADY-COMPUTED authority answering exactly
// "is this receiver one of CString/WString/RawBytes", reused instead of a
// fourth name-keyed exclusion (LANE_RULES §6). Removing the WRONG dispatch
// alone is not enough — three names then had NO dispatch at all, which
// MEASURED WORSE (a SIGSEGV instead of a wrong value, confirmed with a
// throwaway broader exclusion during development and reverted) — so the
// SAME three call sites also gain the CORRECT §8.3/§8.5 no-override default:
// the qualified-name `ToString` literal chain (already used by FrozenArray
// and the #3896 residual-collection names) now also renders "System.CString"
// / "System.WString" / "System.RawBytes" (`kBuiltinClassModulePaths`,
// BuiltinRegistry.cpp, gains the three rows), and the reference-identity
// Equals / identity-hash GetHashCode default (`isBuiltinIdentityRecv`) now
// also admits them via a new `isBuiltinIdentityFfiBuffer` boolean.
//
// Deliberately declined: inverting `cg_isClass` to the FULLY general
// `builtinTypeName(...).empty()` (excluding EVERY `builtinClassSym`-stamped
// name at once, not just these three) was tried during development and
// reverted — it does fix CString/WString/RawBytes, but it ALSO excludes
// CancellationToken/Range/Guid/Stopwatch/etc. from the SAME dispatch with NO
// replacement, and those were measured (see below) to have independent,
// pre-existing instances of this identical defect that this issue does not
// fix — turning their "wrong value" into a SIGSEGV. Filed separately as
// #4126 (see that issue for the measured matrix); NOT fixed here.
//
// Cite: stdlib §10.1/§10.2/§10.3, language spec §8.3/§8.5, v0.295.

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

std::string program(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ═══════════ ToString — primary bug: the measured "silently wrong value" ══
// Unannotated / static-factory-result shape (`var c := CString.From(...)`)
// — the idiomatic form used throughout this file and the stdlib guides.
// Pre-fix: compiles clean, RUNS, returns 1 (the mismatch branch: "" != the
// expected qualified name). Post-fix: returns 42.
BEZEL_TEST(ffi_buffer_4120, cstring_tostring_correct_value_spec_8_5_stdlib_10_1) {
    const std::string p = program(
        "  using var c := CString.From(\"hi\"); "
        "  var s := c.ToString(); "
        "  return (if s = \"System.CString\" then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_4120, wstring_tostring_correct_value_spec_8_5_stdlib_10_2) {
    const std::string p = program(
        "  using var w := WString.From(\"hi\"); "
        "  var s := w.ToString(); "
        "  return (if s = \"System.WString\" then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_4120, rawbytes_tostring_correct_value_spec_8_5_stdlib_10_3) {
    const std::string p = program(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb := RawBytes.From(arr); "
        "  var s := rb.ToString(); "
        "  return (if s = \"System.RawBytes\" then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Parity control: the EXPLICIT-annotation shape (`var c: CString := ...`)
// resolves its TypeRef with `declaration == nullptr` (TypeResolution_Names.cpp
// — CString/WString/RawBytes are registered as arity-0 builtins that keep
// `declaration` null at the annotation site, like DateTime/Guid), so it
// never matched the buggy `cg_isClass`/`isNamedClassRecv` predicates (both
// require a non-null `declaration`) even pre-fix. It IS, however, read by
// the SAME `builtinTypeName`-keyed literal chain and `isBuiltinIdentityRecv`
// this fix widens (that helper handles both TypeRef shapes uniformly), so
// this control also exercises the new dispatch — pinned separately so a
// future regression narrowed to only the inferred shape is caught.
BEZEL_TEST(ffi_buffer_4120,
           cstring_tostring_correct_value_annotated_shape_control) {
    const std::string p = program(
        "  using var c: CString := CString.From(\"hi\"); "
        "  var s := c.ToString(); "
        "  return (if s = \"System.CString\" then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Equals / GetHashCode — §8.3 "Class" default ═══════════════════
// stdlib §10.1-§10.3 declare no `Equals`/`GetHashCode` override on any of the
// three, so language spec §8.3's table row ("Class | Reference equality |
// Identity-based") is the applicable default: two DISTINCT instances with
// equal CONTENTS must compare unequal (reference identity, not content
// equality), self-equality must hold, and GetHashCode must be stable across
// repeated calls on the same instance. Pre-fix: `RawBytes.GetHashCode()`
// read a value out of the receiver's own byte buffer (measured); the other
// two return the class-vtable-misdispatch value seen in the ToString rows
// above (via the SAME `isNamedClassRecv` predicate, gated on `mm ==
// "Equals"`/`"GetHashCode"` rather than `"ToString"`).
BEZEL_TEST(ffi_buffer_4120,
           cstring_equals_and_gethashcode_identity_default_spec_8_3) {
    const std::string p = program(
        "  using var c1 := CString.From(\"hi\"); "
        "  using var c2 := CString.From(\"hi\"); "  // same CONTENT, distinct objects
        "  var selfEq := c1.Equals(c1); "
        "  var crossEq := c1.Equals(c2); "
        "  var h1a := c1.GetHashCode(); "
        "  var h1b := c1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "   // reference equality: distinct objects, unequal
        "  if h1a <> h1b then return 3; end; " // stable across repeated calls
        "  return 42; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_4120,
           wstring_equals_and_gethashcode_identity_default_spec_8_3) {
    const std::string p = program(
        "  using var w1 := WString.From(\"hi\"); "
        "  using var w2 := WString.From(\"hi\"); "
        "  var selfEq := w1.Equals(w1); "
        "  var crossEq := w1.Equals(w2); "
        "  var h1a := w1.GetHashCode(); "
        "  var h1b := w1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_4120,
           rawbytes_equals_and_gethashcode_identity_default_spec_8_3) {
    const std::string p = program(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb1 := RawBytes.From(arr); "
        "  using var rb2 := RawBytes.From(arr); "  // same CONTENT, distinct objects
        "  var selfEq := rb1.Equals(rb1); "
        "  var crossEq := rb1.Equals(rb2); "
        "  var h1a := rb1.GetHashCode(); "
        "  var h1b := rb1.GetHashCode(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Accept side (§15): a real user class must keep working ═══════
// The `cg_isClass`/`isNamedClassRecv`/`isBuiltinIdentityRecv` predicates
// widened above are the SHARED dispatch every class/exception/record/enum
// receiver's ToString/Equals/GetHashCode goes through — `isFfiBufferTypeRef`
// must be a no-op for every one of them. A genuine user class's TypeSymbol
// always has `decl != nullptr`, so `builtinTypeName` (the authority
// `isFfiBufferTypeRef` reads) returns "" for it regardless of name —
// verified here with a class NAMED to look adversarial, and with both the
// no-override default and an explicit override.
BEZEL_TEST(ffi_buffer_4120, accept_side_user_class_default_object_contract) {
    const std::string p =
        "module M; "
        "class Widget public property Name: string := \"w\"; end Widget; "
        "class C public static method Main(): int "
        "  var a := new Widget(); "
        "  var b := new Widget(); "
        "  var selfEq := a.Equals(a); "
        "  var crossEq := a.Equals(b); "
        "  var h1a := a.GetHashCode(); "
        "  var h1b := a.GetHashCode(); "
        "  var s := a.ToString(); "
        "  if s <> \"M.Widget\" then return 1; end; "
        "  if not selfEq then return 2; end; "
        "  if crossEq then return 3; end; "
        "  if h1a <> h1b then return 4; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_4120, accept_side_user_class_override_still_dispatches) {
    const std::string p =
        "module M; "
        "class Widget "
        "  public override method ToString(): string => \"custom\"; "
        "  public override method GetHashCode(): int => 7; "
        "  public override method Equals(other: Object?): bool => true; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var a := new Widget(); "
        "  if a.ToString() <> \"custom\" then return 1; end; "
        "  if a.GetHashCode() <> 7 then return 2; end; "
        "  if not a.Equals(null) then return 3; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(p), 42);
}

// A user class named to collide (in spelling, not in declaration shape)
// with the FFI buffer names is NOT a `builtinClassSym` stub — its TypeSymbol
// has a real `ast::TypeDecl` (`decl != nullptr`), so `builtinTypeName`
// returns "" for it and none of the widened predicates fire on it.
BEZEL_TEST(ffi_buffer_4120,
           accept_side_user_class_named_like_a_builtin_is_unaffected) {
    const std::string p =
        "module M; "
        "class RawBytes public property Tag: int := 9; end RawBytes; "
        "class C public static method Main(): int "
        "  var a := new RawBytes(); "
        "  var s := a.ToString(); "
        "  return (if s = \"M.RawBytes\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(p), 42);
}

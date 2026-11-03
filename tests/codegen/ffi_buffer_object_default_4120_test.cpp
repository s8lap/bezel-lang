// Issue #4120 — the `CString` / `WString` / `RawBytes` Object-contract
// default (`stdlib §10.1` / `§10.2` / `§10.3`), pinned by the RENDERED TEXT
// of the issue's own repro programs.
//
// THE DEFECT (fixed by `narrow #4120`, commit 1cd532ea9, whose subject names
// the issue in PROSE and therefore closes nothing on GitHub).
// `SemaContext::builtinClassSym` stamps `kind = ast::TypeDeclKind::Class`
// UNCONDITIONALLY for every built-in it mints, and these three have no
// dedicated `.ToString()` interceptor in `Emitter::tryLowerBuiltinClassCall`,
// so a no-arg `.ToString()` fell through to `lowerCallDispatch`'s generic
// universal-Object dispatch, where `cg_isClass` / `isNamedClassRecv` matched
// them — misreading each type's raw buffer/pointer payload as a class-vtable
// object (vtable slots 3/4/5).
//
// MEASURED on the pre-fix tree (the parent of 1cd532ea9), by the issue's own
// repro programs, compiled clean with NO diagnostic at any severity:
//   CString.From("hi").ToString()  -> ""          (Length 0)
//   WString.From("hi").ToString()  -> ""          (Length 0)
//   RawBytes.From(arr).ToString()  -> garbage     (Length 3, non-string bytes)
// The correct answer is the §8.5 Object-contract default — the type's
// qualified name — because the type lives in the `System` module
// (`stdlib §10`'s preamble): "System.CString", "System.WString",
// "System.RawBytes".
//
// WHY THIS FILE IS NOT A COPY of `ffi_buffer_object_contract_4120_test.cpp`
// (the `narrow #4120` commit's own rows): that file asserts an EXIT CODE per
// cell, so each row can only say "the string was or was not the expected
// one". This file runs the issue's THREE repro programs VERBATIM and pins
// their exact stdout — the `Length` line AND the rendered name — so the
// regression oracle is the text a user sees, not a boolean.
//
// The oracle is a POSITIVE one: the exact stdout of a program that compiles
// and runs. There is no negative assertion here (no "no diagnostic
// containing X"), so `scripts/check_vacuous_tests.py` has nothing to report.
//
// Cite: language spec §8.3 / §8.5 (v0.295), stdlib §10.1 / §10.2 / §10.3,
// stdlib §10 preamble ("these types live in the `System` module").

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

std::string program(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// The issue's first repro, verbatim: a `CString` renders its §8.5
// Object-contract default. Pre-fix the two lines were "0" and "".
BEZEL_TEST(ffi_buffer_object_default_4120,
           cstring_renders_the_system_qualified_name_spec_8_5_stdlib_10_1) {
    auto r = runAot(program(
        "  using var c := CString.From(\"hi\"); "
        "  var s := c.ToString(); "
        "  Console.WriteLine(s.Length); "
        "  Console.WriteLine(s); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("14\nSystem.CString\n"));
}

// The issue's third repro, verbatim: a `WString` renders the same default.
// Pre-fix the two lines were "0" and "".
BEZEL_TEST(ffi_buffer_object_default_4120,
           wstring_renders_the_system_qualified_name_spec_8_5_stdlib_10_2) {
    auto r = runAot(program(
        "  using var w := WString.From(\"hi\"); "
        "  var s := w.ToString(); "
        "  Console.WriteLine(s.Length); "
        "  Console.WriteLine(s); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("14\nSystem.WString\n"));
}

// The issue's second repro, verbatim: a `RawBytes` renders the same default.
// Pre-fix the two lines were "3" and three garbage bytes — the receiver's
// OWN byte buffer read as a string.
BEZEL_TEST(ffi_buffer_object_default_4120,
           rawbytes_renders_the_system_qualified_name_spec_8_5_stdlib_10_3) {
    auto r = runAot(program(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb := RawBytes.From(arr); "
        "  var s := rb.ToString(); "
        "  Console.WriteLine(s.Length); "
        "  Console.WriteLine(s); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("15\nSystem.RawBytes\n"));
}

// §8.3's Class default is REFERENCE IDENTITY for a type that declares no
// override, so `Equals` / `GetHashCode` on these three must NOT read the
// receiver's payload. The issue's own filing measured
// `RawBytes.From(arr).GetHashCode()` reading a value out of the receiver's
// OWN byte buffer. This row pins the three names' identity default by the
// rendered text: two DISTINCT `RawBytes` over the SAME bytes are UNEQUAL
// (reference identity), and each is equal to itself.
BEZEL_TEST(ffi_buffer_object_default_4120,
           ffi_buffer_equals_is_reference_identity_spec_8_3) {
    auto r = runAot(program(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb1 := RawBytes.From(arr); "
        "  using var rb2 := RawBytes.From(arr); "
        "  Console.WriteLine(rb1.Equals(rb1)); "
        "  Console.WriteLine(rb1.Equals(rb2)); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

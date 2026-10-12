// Codegen / FFI read-back — `string.From(c: CString)` was UNCOMPILABLE, and the
// raw-pointer twins `string.FromCStringPointer` / `string.FromWStringPointer`
// did not exist at all. Issue #2734.
//
// Spec: language spec §32.6 (String marshalling), v0.240 —
//   "`string.From(c)` reads from a caller-owned `CString` and produces a Bezel
//    `string`; the original is unaffected and is still subject to the caller's
//    disposal obligation. `string.FromCStringPointer(p)` is the same operation
//    on a raw `IntPtr`."
// stdlib §9.6 (`String`) declares the four conversions:
//   public static method From(value: CString): string;
//   public static method From(value: WString): string;
//   public static method FromCStringPointer(p: IntPtr): string;
//   public static method FromWStringPointer(p: IntPtr): string;
// stdlib §10.4 (Conversions to Bezel types) fixes their semantics —
//   "copy the contents into a fresh Bezel `string`. The source `CString` or
//    `WString` retains its ownership and disposal obligation."
//   "`string.FromCStringPointer(IntPtr.Zero)` and
//    `string.FromWStringPointer(IntPtr.Zero)` raise `ArgumentNullException`."
// stdlib §3.2 (`Encoding`), v0.136, supplies the strict fault model the shared
// decoders implement ("malformed or unrepresentable input is a defined fault,
// never a silent substitution" → `FormatException`).
//
// THE BUG (three distinct holes, one per conversion family):
//
//  1. `string.From(c: CString)` — the codegen arm in LlvmBackend_Calls2.cpp
//     dispatched on ARGUMENT COUNT, not argument type: every 1-arg
//     `string.From(x)` was claimed by the `From(char)` lowering, which coerces
//     its argument to i32. A CString lowers to a bare pointer, so the emitted
//     IR was `sext ptr %2 to i32` — "SExt only operates on integer" — and
//     compilation aborted. Its comment ("string.From(c) is equivalent to
//     char.ToString(c)") was true when written and went false when §32.6 added
//     the FFI overloads. The same family as #2757 / #2763 / #2773 / #2787: a
//     predicate that enumerates one case and is later reached by others.
//
//  2. `string.FromCStringPointer` / `FromWStringPointer` were absent from
//     sema's closed `string`-static set, so §32.6's own `getenv` read-back
//     idiom was rejected outright ("type 'string' has no static method or
//     property 'FromCStringPointer'"), and had no lowering either.
//
//  3. There was NO runtime entry point for this direction at all: the reverse
//     (`CString.From(s)` → bz_string_to_utf8z, `WString.From(s)` →
//     bz_string_to_utf16z) existed; the forward did not.
//
// Two collateral defects the above uncovered, fixed with it:
//  4. sema's #1145 `String.From` overload guard matched only the TypeRef shape
//     a FACTORY call produces (declaration = builtinClassSym) — so
//     `string.From(c)` on a CString-typed PARAMETER (declaration = null,
//     annotation head only) was falsely rejected as "no matching overload".
//  5. `namedValueBuiltinRef` (ExprTypecheck.cpp) filled the AST
//     `QualifiedName::parts` — a vector of NON-OWNING `std::string_view` — with
//     a temporary `std::string`, so every `IntPtr.Zero` expression carried a
//     DANGLING type name. Silent while nothing read it; the §10.4 null-pointer
//     guard reads it, and compared garbage.
//
// THE FIX. Gate on the argument's sema type (`builtinTypeName`), not on arity;
// add `bz_string_from_utf8z` / `bz_string_from_utf16z`, which route through the
// EXISTING strict decoders (lifted out of `bz_encoding_utf8_get_string` /
// `bz_encoding_utf16_get_string` into shared static helpers rather than copied,
// so the well-formedness rules exist once); guard the raw-pointer forms with
// ArgumentNullException per §10.4. The buffer is READ only — no free, no
// retention — so §32.6's "still subject to the caller's disposal obligation"
// holds.
//
// OBSERVABILITY. Every behavioral case runs under runAotChecked (ASan/LSan): a
// leak or a double-free of the CString buffer flips `r.ran` to false, and the
// exit code pins the VALUE. Cases A–F fail without the fix by failing to
// compile at all (`r.ran` false, or a sema rejection); G–I pin the normative
// exception behavior; J pins §28.3 temporary reclaim; the K controls pin that
// the char / 3-arg overloads and the §3.2 Encoding decoders are unchanged.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {


// Diagnostic TEXT, not merely "did not run": runAot synthesizes an Error for a
// runtime SIGSEGV too, so a bare rejects() cannot tell a genuine reject from a
// crash (and the pre-fix failure mode for these very programs WAS an abort).
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (A) THE HEADLINE — the issue's exact repro. Pre-fix: `sext ptr to i32`,
// "IR verification failed", no executable at all.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, from_cstring_compiles_spec_32_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var c := CString.From(\"abc\"); "
        "  var s := string.From(c); "
        "  return s.Length; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// (B) VALUES, not just "it compiles" — a full round trip over real non-ASCII
// text: a 2-byte code point (U+00E9), a 3-byte one (U+2014) and one ABOVE the
// BMP (U+1D11E, which is a surrogate PAIR in UTF-16). §10.4: the conversion
// "copies the contents into a fresh Bezel `string`", so
// `string.From(CString.From(s)) = s`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, cstring_round_trip_astral_stdlib_10_4) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var orig := \"h\xC3\xA9llo \xE2\x80\x94 \xF0\x9D\x84\x9E ok\"; "
        "  using var c := CString.From(orig); "
        "  var back := string.From(c); "
        "  if back <> orig then return 1; end; "
        "  if back.Length <> orig.Length then return 2; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (B2) The WString twin — the same text through the UTF-16LE buffer, so the
// astral code point exercises the surrogate-pair decode path.
BEZEL_TEST(codegen_string_from_ffi_2734, wstring_round_trip_astral_stdlib_10_4) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var orig := \"h\xC3\xA9llo \xE2\x80\x94 \xF0\x9D\x84\x9E ok\"; "
        "  using var w := WString.From(orig); "
        "  var back := string.From(w); "
        "  if back <> orig then return 1; end; "
        "  if back.Length <> orig.Length then return 2; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (C) The PARAMETER shape (collateral defect 4). A `c: CString` annotation
// resolves to a null-declaration Named TypeRef, which #1145's guard did not
// recognize — so this spelling was rejected as "no matching overload" even
// though stdlib §9.6 declares exactly that overload.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, from_cstring_parameter_stdlib_9_6) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method read(c: CString): string "
        "    return string.From(c); "
        "  end read; "
        "  public static method readW(w: WString): string "
        "    return string.From(w); "
        "  end readW; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"h\xC3\xA9llo\"); "
        "    using var w := WString.From(\"h\xC3\xA9llo\"); "
        "    if read(c) <> \"h\xC3\xA9llo\" then return 1; end; "
        "    if readW(w) <> \"h\xC3\xA9llo\" then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (D) OWNERSHIP — §32.6: "the original is unaffected and is still subject to
// the caller's disposal obligation". The CString is read FOUR times and is
// still readable afterwards; the `using var` owes exactly one free. Under
// ASan/LSan a consumed-or-freed source shows as a double-free or a
// use-after-free, and a retained source as a leak: a test that only compared
// the returned text would pass with either latent underneath.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, source_not_consumed_spec_32_6) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method readTwice(c: CString): int "
        "    var a := string.From(c); "
        "    var b := string.From(c); "
        "    if a <> b then return -1; end; "
        "    return a.Length; "
        "  end readTwice; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"h\xC3\xA9llo\"); "
        "    var s1 := string.From(c); "
        "    var s2 := string.From(c); "
        "    if s1 <> s2 then return 1; end; "
        "    if readTwice(c) <> 5 then return 2; end; "
        // §10.1: Length is the BYTE count — 6 for the 5-code-point "héllo".
        "    if c.Length <> 6 then return 3; end; "
        "    if string.From(c) <> \"h\xC3\xA9llo\" then return 4; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (D2) The same read in a LOOP — the scope-end release must fire once per
// iteration (the slot is an entry-block alloca reused by every iteration), and
// the source buffer must survive all of them.
BEZEL_TEST(codegen_string_from_ffi_2734, repeated_read_no_leak_spec_32_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var c := CString.From(\"h\xC3\xA9llo\"); "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 40 do "
        "    var s := string.From(c); "
        "    if s.Length = 5 then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ---------------------------------------------------------------------------
// (E) `string.FromCStringPointer(p)` — §32.6's borrowed-pointer shape, read
// through a pointer the Bezel side does NOT own. `strchr` returns a pointer
// INTO the CString's own buffer, so nothing is allocated or freed by the
// conversion; the CString's single disposal obligation is unchanged.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, from_cstring_pointer_spec_32_6) {
    const std::string src =
        "module M; "
        "extern \"c\" "
        "  public method strchr(s: CString, c: int): IntPtr; "
        "end; "
        "class C public static method Main(): int "
        "  using var cs := CString.From(\"abXYZ\xC3\xA9\"); "
        "  var p := strchr(cs, 88); "                    // 88 = 'X'
        "  if p = IntPtr.Zero then return 9; end; "
        "  var s := string.FromCStringPointer(p); "
        "  if s <> \"XYZ\xC3\xA9\" then return 1; end; "
        // the source is untouched: still readable, still owes one free
        "  if string.From(cs) <> \"abXYZ\xC3\xA9\" then return 2; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (F) `string.FromWStringPointer(p)` — the UTF-16LE twin. The source is a
// RawBytes buffer (stdlib §10.3: `From` copies the bytes verbatim, `Pointer`
// is its address) holding the UTF-16LE encoding plus a terminating 0x0000 code
// unit, which is exactly what §10.4 says the conversion reads to.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, from_wstring_pointer_spec_32_6) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var raw := Encoding.UTF16.GetBytes(\"h\xC3\xA9\xF0\x9D\x84\x9E\"); "
        "  raw.Add(0); raw.Add(0); "                    // the 0x0000 terminator
        "  using var rb := RawBytes.From(raw); "
        "  var w := string.FromWStringPointer(rb.Pointer); "
        "  if w <> \"h\xC3\xA9\xF0\x9D\x84\x9E\" then return 1; end; "
        "  if w.Length <> 3 then return 2; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (G/H) stdlib §10.4, normative: "`string.FromCStringPointer(IntPtr.Zero)` and
// `string.FromWStringPointer(IntPtr.Zero)` raise `ArgumentNullException`."
// Asserted by CATCHING it — a SIGSEGV would leave `raised` false (and flip
// r.ran), so this cannot be satisfied by a crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, null_pointer_raises_stdlib_10_4) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method cRaises(): bool "
        "    var raised := false; "
        "    try "
        "      var s := string.FromCStringPointer(IntPtr.Zero); "
        "      _ := s.Length; "
        "    catch ArgumentNullException => raised := true; "
        "    end; "
        "    return raised; "
        "  end cRaises; "
        "  public static method wRaises(): bool "
        "    var raised := false; "
        "    try "
        "      var s := string.FromWStringPointer(IntPtr.Zero); "
        "      _ := s.Length; "
        "    catch ArgumentNullException => raised := true; "
        "    end; "
        "    return raised; "
        "  end wRaises; "
        "  public static method Main(): int "
        "    if not cRaises() then return 1; end; "
        "    if not wRaises() then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (I) Ill-formed input. §10.4 does not state what happens when the buffer is
// not well-formed in its encoding; routing through the SAME strict decoders
// `Encoding.UTF8/UTF16.GetString` use inherits stdlib §3.2's model ("malformed
// or unrepresentable input is a defined fault, never a silent substitution")
// — a catchable FormatException. That reading is recorded with the fix. A
// CString of arbitrary bytes is legitimate: §10.1 says `FromBytes` "performs
// NO validation: the bytes are taken verbatim".
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, ill_formed_raises_stdlib_3_2) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method raises(): bool "
        "    var raised := false; "
        "    var b := Encoding.UTF8.GetBytes(\"A\"); "
        "    b.Add(255); "                    // 0xFF — never a valid UTF-8 byte
        "    try "
        "      using var c := CString.FromBytes(b); "
        "      var s := string.From(c); "
        "      _ := s.Length; "
        "    catch FormatException => raised := true; "
        "    end; "
        "    return raised; "
        "  end raises; "
        "  public static method Main(): int "
        "    if not raises() then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (J) The issue's SECOND spelling — the nested temporary. §28.3 v0.240: an
// unbound owned temporary "is disposed at the end of the full expression in
// which it is produced, after every use of it within that expression has
// completed". Without the #2728/#2733 registration the CString buffer simply
// leaked (LSan: 13 bytes for "abc"), which LSan turns into r.ran == false.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_string_from_ffi_2734, nested_temporary_reclaimed_spec_28_3) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var count := 0; "
        "  var i := 0; "
        "  while i < 20 do "
        "    var s := string.From(CString.From(\"abc\")); "
        "    if s = \"abc\" then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return count; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// (J2) The same temporary on the UNWIND edge: the inline CString is minted and
// the conversion then THROWS. The reclaim must reach it from the exception
// edge (the disposeStack_ half of the #2733 registration), not only from the
// full-expression boundary — 20 iterations of a leak would be unmissable.
BEZEL_TEST(codegen_string_from_ffi_2734, temporary_reclaimed_on_throw_spec_28_3) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method raises(): bool "
        "    var raised := false; "
        "    using var b := Encoding.UTF8.GetBytes(\"A\"); b.Add(255); "
        "    try "
        "      var s := string.From(CString.FromBytes(b)); "
        "      _ := s.Length; "
        "    catch FormatException => raised := true; end; "
        "    return raised; "
        "  end raises; "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 20 do "
        "      if raises() then count := count + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// ---------------------------------------------------------------------------
// (K) CONTROLS — what the fix must NOT break.
// ---------------------------------------------------------------------------

// K1: the `From(char)` overload the buggy arm was written for, and the 3-arg
// `From(buffer, start, count)` that sits directly below it (stdlib §9.6).
BEZEL_TEST(codegen_string_from_ffi_2734, char_and_buffer_overloads_stdlib_9_6) {
    const std::string src =
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  if string.From('Z') <> \"Z\" then return 1; end; "
        "  using var arr := new Array<char>(); "
        "  arr.Add('w'); arr.Add('x'); arr.Add('y'); arr.Add('z'); "
        "  if string.From(arr, 1, 2) <> \"xy\" then return 2; end; "
        "  if string.Repeat('A', 3) <> \"AAA\" then return 3; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// K2: the #1145 no-overload reject is INTACT for the shapes §9.6 really has no
// overload for — a 1-arg `Array<char>` and a `RawBytes` (§10.3 directs callers
// through `Array<byte>` and a decoder, not through `string.From`). Widening
// the guard to the annotation TypeRef shape must not have widened it to these.
BEZEL_TEST(codegen_string_from_ffi_2734, unrelated_one_arg_still_rejected_1145) {
    const char* kNeedle = "no matching overload of `String.From`";
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<char>(); arr.Add('h'); "
        "  var t := string.From(arr); "
        "  return t.Length; "
        "end Main; end C;", kNeedle));
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class C public static method Main(): int "
        "  using var rb := RawBytes.From(Encoding.UTF8.GetBytes(\"abc\")); "
        "  var t := string.From(rb); "
        "  return t.Length; "
        "end Main; end C;", kNeedle));
}

// K3: the raw-pointer forms take exactly one `IntPtr` (stdlib §9.6). A
// `string` argument must NOT be silently reinterpreted as a C buffer — that
// would decode the bz.string header as text and yield garbage, which is
// strictly worse than a diagnostic.
BEZEL_TEST(codegen_string_from_ffi_2734, pointer_form_needs_intptr_stdlib_9_6) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class C public static method Main(): int "
        "  var s := string.FromCStringPointer(\"abc\"); "
        "  return s.Length; "
        "end Main; end C;",
        "no matching overload of `String.FromCStringPointer`"));
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class C public static method Main(): int "
        "  var s := string.FromWStringPointer(\"abc\"); "
        "  return s.Length; "
        "end Main; end C;",
        "no matching overload of `String.FromWStringPointer`"));
}

// K4: the §3.2 `Encoding` decoders are unchanged by the lift of their bodies
// into the shared helpers — the round trips, the UTF-16 BOM consumption, and
// the strict faults all still hold. (The UTF-16 BOM policy deliberately stays
// in `bz_encoding_utf16_get_string`, NOT in the shared helper: §10.4's
// pointer read has no BOM rule, and consuming one there would silently drop a
// leading U+FEFF from a WString round trip — see K5.)
//
// Runs under plain runAot (LSan off): a bound `Encoding.*.GetString` result
// leaks today — that decoder is missing from `isPlusOneStringProducer`, the
// same #2228 family, filed separately — and this case is a control for DECODER
// BEHAVIOR, not for memory. The memory assertions live in cases A–J above.
BEZEL_TEST(codegen_string_from_ffi_2734, encoding_decoders_unchanged_stdlib_3_2) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method utf8BadRaises(): bool "
        "    var raised := false; "
        "    using var b := Encoding.UTF8.GetBytes(\"A\"); b.Add(255); "
        "    try _ := Encoding.UTF8.GetString(b); "
        "    catch FormatException => raised := true; end; "
        "    return raised; "
        "  end utf8BadRaises; "
        "  public static method utf16OddRaises(): bool "
        "    var raised := false; "
        "    using var b := Encoding.UTF16.GetBytes(\"A\"); b.Add(1); "
        "    try _ := Encoding.UTF16.GetString(b); "
        "    catch FormatException => raised := true; end; "
        "    return raised; "
        "  end utf16OddRaises; "
        "  public static method Main(): int "
        "    var orig := \"h\xC3\xA9llo \xF0\x9D\x84\x9E\"; "
        "    using var b8 := Encoding.UTF8.GetBytes(orig); "
        "    var d8 := Encoding.UTF8.GetString(b8); "
        "    if d8 <> orig then return 1; end; "
        "    using var b16 := Encoding.UTF16.GetBytes(orig); "
        "    var d16 := Encoding.UTF16.GetString(b16); "
        "    if d16 <> orig then return 2; end; "
        // §3.2: GetString consumes one leading little-endian BOM (FF FE).
        "    using var bom := Encoding.UTF8.GetBytes(\"\"); "
        "    bom.Add(255); bom.Add(254); "
        "    var i := 0; "
        "    while i < b16.Count do bom.Add(b16[i]); i := i + 1; end; "
        "    var dbom := Encoding.UTF16.GetString(bom); "
        "    if dbom <> orig then return 3; end; "
        "    if not utf8BadRaises() then return 4; end; "
        "    if not utf16OddRaises() then return 5; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// K5: a leading U+FEFF is ORDINARY CONTENT for the §32.6 read-back — §10.4
// reads "16-bit code units starting at the pointer until a null terminator is
// found" and states no BOM rule, and §32.6 says the copy is of the buffer's
// contents. Consuming it (as §3.2's Encoding.UTF16 contract does) would make
// `string.From(WString.From(s)) <> s` for such an s. This pins the split.
BEZEL_TEST(codegen_string_from_ffi_2734, wstring_bom_is_content_stdlib_10_4) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var orig := \"\xEF\xBB\xBF" "abc\"; "        // U+FEFF then "abc"
        "  if orig.Length <> 4 then return 9; end; "
        "  using var w := WString.From(orig); "
        "  var back := string.From(w); "
        "  if back <> orig then return 1; end; "
        "  if back.Length <> 4 then return 2; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// K6: `IntPtr.Zero` must still work everywhere it worked before the
// dangling-name fix in `namedValueBuiltinRef` (collateral defect 5) — the
// §32.7 null test and the `IntPtr(long)` / `long(IntPtr)` conversions.
BEZEL_TEST(codegen_string_from_ffi_2734, intptr_zero_and_conversions_spec_32_7) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var z := IntPtr.Zero; "
        "  if z <> IntPtr.Zero then return 1; end; "
        "  var p := IntPtr(1234); "
        "  if p = IntPtr.Zero then return 2; end; "
        "  if long(p) <> 1234 then return 3; end; "
        "  var u := UIntPtr.Zero; "
        "  if u <> UIntPtr.Zero then return 4; end; "
        "  return 0; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

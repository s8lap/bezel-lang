// fix #2825 / #2810 — `CString.Pointer` and `WString.Pointer` (stdlib §10.1
// `CString` / §10.2 `WString`, v0.240) had NO lowering and NO sema typing, so
// the read fell through codegen's permissive member fall-through and the address
// came back NULL.
//
// #2810 and #2825 are ONE defect reported twice, hours apart, from two states of
// the tree.  #2810 calls it a "codegen ICE"; that characterization is wrong and
// #2825's is right — the two `internal:` lines are WARNINGS (the #2547
// member-fall-through note and the #2434 initializer-degrade note), the compile
// SUCCEEDS, and the program runs and hands the caller a null address.  Verified
// on this commit's parent: both issues' repros produce byte-identical diagnostics
// and a zero `Pointer`.
//
// WHY A SILENT NULL IS THE WORST AVAILABLE FAILURE HERE.  stdlib §10.1 names the
// consumer in the same sentence that declares the member: `Pointer` "exposes the
// underlying buffer's address … — the element source for `char*[]` argv-building
// over `OwningBuffer<IntPtr>` (§4.14)".  The null therefore does not stay inside
// Bezel: it is handed to C, which dereferences it.  No Bezel-side diagnostic, and
// the crash happens in foreign code where none of this project's tooling is
// looking.  That is why the rows below prove a VALUE — a real address that C can
// use — and not merely a clean compile or a non-zero integer.
//
// ROOT CAUSE, both halves an ENUMERATION of the three §10 heads:
//   * codegen  src/codegen/LlvmBackend_Exprs.cpp — the `ma->member == "Pointer"`
//     arm tested `RawBytes` alone.  The sibling `.Length` arm directly above it
//     already handled all three, which is exactly the cost of enumerating the
//     family per-arm instead of naming it once.
//   * sema     src/sema/ExprTypecheck_Calls.cpp — the `Pointer: IntPtr` typing
//     was `RawBytes`-only on BOTH axes: one of three heads, and one of the two
//     TypeRef shapes a §10 builtin arrives in (#2809/#2734: the FACTORY result
//     `CString.From(s)` carries declaration = builtinClassSym; the ANNOTATION
//     `c: CString` on a parameter/field carries declaration = null and only the
//     head name).
// Both now read the family through `common::isFfiBufferBuiltinName` — the single
// home of the {CString, WString, RawBytes} set (#2809/#2757) — so there is no
// sixth hand-maintained name list.
//
// LIFETIME, settled by the documents, not guessed.  spec §32.10 "Boundary
// contract" (v0.240) lists among the programmer's responsibilities: "For
// addresses of Bezel-owned mutable storage (`RawBytes.Pointer`,
// `CString`/`WString.Pointer`, `OwningBuffer<T>.DangerousPointer()` — stdlib
// §10, §4.14): native retention of such an address past the synchronous call is
// a use-after-free hazard no rule in this chapter can track — validity ends at a
// disposal the native side is never told about."  And §32.12 item 7: "No pinning
// exists or is needed: Bezel has no moving collector (§28); every address the
// language exposes is stable until its owner's disposal."  So the address is an
// ALIAS of the receiver's buffer — never a fresh allocation, nothing new owned,
// nothing new freed — and a read after the owner's disposal is the programmer's
// obligation, not a rule the compiler is required to enforce.  Rows I and J pin
// the alias property in both directions (identity, and a C-side WRITE landing in
// the CString's own buffer); row K pins that no ownership is created.
//
// FAIL-WITHOUT, MEASURED.  With `src/codegen/LlvmBackend_Exprs.cpp` and
// `src/sema/ExprTypecheck_Calls.cpp` reverted to their pre-fix state and this
// file unchanged: rows A–K fail (13 in-module rows: 10 failed, and row J failed
// only after its non-null conjunct was added — see the note there), and rows L
// and M pass either way.  Those two are the RawBytes control and the
// user-type-exclusion control: passing before the fix is exactly what they are
// for.  The two cross-module rows in tests/module/xmod_ffi_pointer_2825_test.cpp
// both fail pre-fix as well.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The churn tail every leak row shares: an allocation AFTER the measured loop,
// so no register still holds the last iteration's buffer when LeakSanitizer
// runs.  A one-shot `Main` is not a reliable LSan oracle.
const char* kChurn =
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 90; end; ";

}  // namespace

// ===========================================================================
// A — #2825's own repro, byte for byte.  A `using var`-bound `CString`
// receiver; pre-fix the two `internal:` warnings printed and the program
// returned 11 (a NULL address).
// per stdlib §10.1, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_nonnull_bound_spec_10_1) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    var p: IntPtr := c.Pointer; "
        "    if long(p) <> 0 then return 77; end; "
        "    return 11; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// ===========================================================================
// B — the same for `WString`, which #2810 could only INFER ("the member has no
// handler at all").  It is real: measured, same two warnings, same null.
// per stdlib §10.2, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, wstring_pointer_nonnull_bound_spec_10_2) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var w := WString.From(\"abc\"); "
        "    var p: IntPtr := w.Pointer; "
        "    if long(p) <> 0 then return 78; end; "
        "    return 12; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 78);
}

// ===========================================================================
// C — THE VALUE ROW, and the hazard shape stdlib §10.1 names: the address is
// handed to C and C dereferences it.  libc `strlen` walks the buffer to its
// null terminator and returns 5 for "hello".  A non-zero-but-wrong address
// would segfault or return garbage here; the pre-fix NULL segfaults inside
// libc, which is precisely the failure mode that never reaches a Bezel
// diagnostic.
// per stdlib §10.1 / spec §32.6, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_read_by_c_spec_32_6) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"hello\"); "
        "    return int(strlen(c.Pointer)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// D — the `WString` value row.  `string.FromWStringPointer(p)` (stdlib §10.4,
// implemented by #2734) reads 16-bit code units through the pointer until the
// terminating zero unit, so a round-trip back to the original text proves both
// the address AND that it points at UTF-16 content, not UTF-8 or a header.
// per stdlib §10.2 / §10.4, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, wstring_pointer_roundtrip_spec_10_4) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var w := WString.From(\"world\"); "
        "    var s := string.FromWStringPointer(w.Pointer); "
        "    if s = \"world\" then return 43; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// E — the PARAMETER receiver: the ANNOTATION TypeRef shape (`c: CString`,
// declaration == null).  This is the shape #2809 showed falls through
// declaration-only predicates, and it is the shape a helper that builds an argv
// array actually has.  Both the sema typing and the codegen arm must read it.
// per stdlib §10.1 / spec §32.5, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_param_receiver_spec_10_1) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class C "
        "  public static method Addr(c: CString): IntPtr "
        "    return c.Pointer; "
        "  end Addr; "
        "  public static method Main(): int "
        "    using var c := CString.From(\"hello\"); "
        "    return int(strlen(C.Addr(c))); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// F — the FIELD receiver, the other half of the annotation shape (`owned _c:
// CString` on a class).  §32.13's wrapper posture — "one wrapper class owns both
// the buffer and the native registration" (§32.10) — is the recorded structural
// mitigation for the retention hazard, so this is the shape the spec itself
// steers users toward; it must work.
// per spec §32.10 / §32.13, stdlib §10.1, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_field_receiver_spec_32_13) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class Holder "
        "  private owned _c: CString; "
        "  public constructor() _c := CString.From(\"hello\"); end constructor; "
        "  public method Ptr(): IntPtr return _c.Pointer; end Ptr; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return int(strlen(h.Ptr())); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// G — the UNBOUND-TEMPORARY receiver (§28.3 *Unbound owned temporaries*: a
// member access is one of the listed borrowing uses).  The pointer is consumed
// inside the same full expression, which is where §28.3 puts the temporary's
// disposal — "after every use of it within that expression has completed" — so
// the read is still in-bounds when `string.FromCStringPointer` copies through it.
//
// fix #2828 / §28.3 (v0.240): the §10 member-read RECEIVER site is now wired
// through `registerFfiExprTemp`, so the temporary is reclaimed at the K8
// boundary — `runAotChecked` (LSan) passes. Before #2828, the slot was
// dropped and the temporary leaked 21 B; #2828's companion row
// (ffi_buffer_member_read_receiver_release_2828_test.cpp:row D) exercises
// the same site in a 20-iteration loop to amplify the leak to LSan-
// observable.
// per spec §28.3 / stdlib §10.4, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_temp_receiver_spec_28_3) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := string.FromCStringPointer(CString.From(\"hello\").Pointer); "
        "    if s = \"hello\" then return 81; end; "
        "    return 15; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 81);
}

// ===========================================================================
// H — the §7.4-NARROWED nullable receiver.  §32.6 declares the "may return
// NULL" FFI shape as `owned CString?`, so the narrowed spelling is the ONLY way
// to reach `.Pointer` on such a value.  §7.4 narrowing deliberately does not
// rewrite `exprType`, so a predicate that reads the type naively misses it.
// per spec §7.4 / §32.6, stdlib §10.1, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_narrowed_nullable_spec_7_4) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method strdup(s: CString): owned CString?; "
        "  public method strlen(s: IntPtr): long; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var src := CString.From(\"hello\"); "
        "    using var d := strdup(src); "
        "    if d <> null then return int(strlen(d.Pointer)); end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// I — THE ALIAS PROPERTY, proven from C's side.  §32.10 calls these "addresses
// of Bezel-owned mutable storage"; the address must therefore be the buffer
// ITSELF, not a copy of it.  `memset(p, 'X', 1)` writes one byte through the
// pointer from C, and `string.From(c)` (stdlib §9.6/§10.4) then reads the
// CString back as "Xbc" — the mutation is visible through the owning Bezel
// value.  A lowering that returned the address of a copy would still be non-null
// and would still pass rows A–H; only this row rejects it.
// per spec §32.10, stdlib §10.1, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_aliases_buffer_spec_32_10) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method memset(s: IntPtr, ch: int, n: ulong): IntPtr; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    _ := memset(c.Pointer, 88, 1); "
        "    var s := string.From(c); "
        "    if s = \"Xbc\" then return 42; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// J — the alias is STABLE: two reads of the same receiver yield the same
// address.  §32.12 item 7, verbatim: "every address the language exposes is
// stable until its owner's disposal".  This also rejects a lowering that
// allocated per read.
//
// ⚠ The non-null conjunct is load-bearing, not belt-and-braces: with only
// `p1 = p2` this row PASSED pre-fix, because both reads degraded to zero and
// `0 = 0` holds.  Caught by running the fail-without measurement rather than
// assuming it — a vacuous row is the failure mode review catches most often.
// per spec §32.12, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, cstring_pointer_stable_across_reads_spec_32_12) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var c := CString.From(\"abc\"); "
        "    var p1 := c.Pointer; "
        "    var p2 := c.Pointer; "
        "    if long(p1) = 0 then return 2; end; "
        "    if p1 = p2 then return 45; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 45);
}

// ===========================================================================
// K — THE OWNERSHIP ROW.  Because the address is an alias and not an
// allocation, reading `.Pointer` must create no disposal obligation: no leak
// (the buffer is still freed exactly once, by its `using var`) and no double
// free (the read must not be mistaken for an owned producer).  20 iterations
// plus a churn allocation, `runAotChecked` = generated-code ASan with
// LeakSanitizer armed, so either direction fails this row.  Both `CString` and
// `WString`, and the value is checked too (5 * 20 = 100).
// per spec §28.3 / §32.10, stdlib §10.1 / §10.2, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, pointer_read_adds_no_ownership_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "extern \"c\" public method strlen(s: IntPtr): long; end; "
        "class C "
        "  public static method Main(): int "
        "    var t: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var c := CString.From(\"hello\"); "
        "      using var w := WString.From(\"hello\"); "
        "      t := t + strlen(c.Pointer); "
        "      if w.Pointer = IntPtr.Zero then return 92; end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// L — THE CONTROL: `RawBytes.Pointer` (stdlib §10.3, working since #985) still
// reads the `ptr` field of the { ptr, len } struct through `bz_rawbytes_ptr`.
// The arm was restructured to serve all three heads, so this row guards the one
// head that already worked.  Passes before AND after the fix, by design.
// per stdlib §10.3, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, rawbytes_pointer_control_spec_10_3) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      var b: Array<byte> := [1, 2, 3]; "
        "      using var rb := RawBytes.From(b); "
        "      if rb.Pointer = IntPtr.Zero then return 92; end; "
        "      t := t + rb.Length; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 60 then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// M — THE OVER-CLAIM CONTROL: a user-declared `class CString` with its OWN
// `Pointer` member keeps its own typing and its own lowering.  §2.3
// "Always-available names" makes `CString` merely auto-imported — "the FFI-
// boundary types `IntPtr`, `UIntPtr`, `CString`, `WString`, `RawBytes` — stdlib
// §10, whose module home is `System`" — not reserved, so a module may declare
// its own.  Both new predicates exclude a TypeRef whose `declaration` carries a
// real user `TypeDecl` (codegen via `builtinTypeName`, sema by mirroring it
// branch for branch), so widening the family from one head to three cannot
// capture a user type that happens to share the name.  A false claim here would
// be worse than the bug this commit fixes: it breaks working code.  Passes
// before AND after the fix.
// per spec §2.3, v0.240
// ===========================================================================
BEZEL_TEST(codegen_ffi_pointer_2825, user_declared_cstring_not_claimed_spec_2_3) {
    auto r = runAot(
        "module M; "
        "class CString "
        "  public readonly property Pointer: int; "
        "  public constructor(p: int) Pointer := p; end constructor; "
        "end CString; "
        "class C "
        "  public static method Main(): int "
        "    using var u := new CString(37); "
        "    return u.Pointer; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 37);
}

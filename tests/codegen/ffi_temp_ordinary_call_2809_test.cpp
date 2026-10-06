// fix #2809 (codegen; spec §32.5 "FFI-compatible types" / §32.6 "String
// marshalling" / §28.3 "Local variables, ownership, and borrows" — its
// *Unbound owned temporaries* subsection; stdlib §10.1 `CString` / §10.2
// `WString` / §10.3 `RawBytes`; all v0.240).
//
// TWO DISTINCT DEFECTS SHARED ONE REPRO, `len(CString.From("abc"))`.
//
// (A) THE MEMORY ERROR — a §10 FFI-boundary receiver in the ANNOTATION TypeRef
//     shape was read as a `%bz.string`.  A `CString`/`WString`/`RawBytes`
//     reaches codegen in two indistinguishable TypeRef shapes: the FACTORY
//     result (`CString.From(s)` — `declaration` = the builtinClassSym stamped
//     by ExprTypecheck_Calls.cpp) and the ANNOTATION (`c: CString` on a
//     parameter or field — `declaration` = null, head name only, per
//     TypeResolution_Names.cpp).  `LlvmBackend_Exprs.cpp`'s declaration-less
//     `.Length` STRING fallback (`isStringFallbackTarget`) is a NEGATIVE list —
//     "not StringBuilder, not Buffer, not FileStream, not a collection =>
//     assume `%bz.string`" — and the three FFI names were its fifth missing
//     category.  So inside the callee `c.Length` lowered to
//     `getelementptr %bz.string, ptr %c, i32 0, i32 1` + `load i64`: an 8-byte
//     read at offset 8..15 of a bare malloc'd `char*` that has no header at
//     all.  Measured pre-fix on the issue's own repro:
//
//       ==...==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8
//       0x...1f is located 2 bytes after 13-byte region [0x...10,0x...1d)
//       allocated by ... bz_string_to_utf8z ... in bz$M.C$len
//
//     (13 = `bz_string_to_utf8z`'s 4*len+1 worst-case buffer for "abc".)  With
//     a longer string the same read stays in bounds and silently returns
//     GARBAGE instead — `len(CString.From("abcd"))` returned a non-4 value.
//     The `using var`-bound spelling always worked, because that TypeRef
//     carries the builtinClassSym declaration and never entered the fallback;
//     only the parameter/field spelling was affected.
//
// (B) THE LEAK — the unbound owned FFI temporary was never reclaimed on the
//     ORDINARY call path.  §28.3 *Unbound owned temporaries*: "Such a temporary
//     is disposed at the **end of the full expression** in which it is
//     produced, after every use of it within that expression has completed",
//     and §32.6: "The `CString` class is for **caller-owned** strings: those
//     that the Bezel side allocated and is responsible for disposing."  The
//     machinery that implements exactly this (`isFfiBufferFactoryCall` +
//     `registerFfiExprTemp` + `flushFfiExprTemps`, #2728/#2733) was wired into
//     the EXTERN argument arm, the bare `ExpressionStmt`/`DiscardStmt` arms and
//     (#2734) `string.From` — but into no general call path.  Measured pre-fix:
//     41 B/iteration leaked from `bz_string_to_utf8z` for an ordinary static
//     call, 17 B/construction for a constructor.  Three argument-building sites
//     needed it and now have it: `buildOrderedArgs` (LlvmBackend_Assign.cpp —
//     local methods AND local constructors), `lowerCall`'s hand-rolled IMPORTED
//     -method arm (LlvmBackend_Calls2.cpp) and `lowerNew`'s hand-rolled
//     IMPORTED-ctor arm (LlvmBackend_Exprs2.cpp); the latter two exist because
//     an imported callee has no reconstructed AST for `buildOrderedArgs` to
//     read (#1221).  The cross-module rows live in
//     tests/module/xmod_ffi_temp_arg_2809_test.cpp.
//
// WHAT EACH ROW OBSERVES.  `runAotChecked` instruments the GENERATED code with
// AddressSanitizer AND turns LeakSanitizer on, so (A) fails as a hard ASan
// report (`ran` = false) and (B) fails as an LSan report — neither can pass
// vacuously.  The leak rows loop 20x and allocate afterwards, because a small
// `Main` is not a reliable LSan oracle: a leftover pointer in a register can
// make LSan call a leaked block reachable.
//
// FAIL-WITHOUT, MEASURED.  With `src/codegen/` reverted to its pre-fix state
// and this file unchanged: 11 of the first 13 rows below fail (the two
// ToBytes/CopyTo rows were added after that measurement and fail pre-fix by
// construction — the internal-degrade error and the 0-instead-of-26).  The two
// that pass either way are the DOUBLE-FREE controls for the arms #2733 and
// #2734 already covered (`extern_arg_temp_not_double_freed`,
// `nested_string_from_not_double_freed`) — they exist to prove the new
// registration does not claim a temporary a pre-existing arm already owns, so
// passing before the fix is exactly what they should do.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The churn tail every leak row shares: an allocation AFTER the measured loop,
// so no register still holds the last iteration's buffer when LSan runs.
const char* kChurn =
    "  var junk: Array<int> := [1, 2, 3]; "
    "  if junk[0] <> 1 then return 90; end; ";

}  // namespace

// ===========================================================================
// (A) THE MEMORY ERROR
// ===========================================================================

// spec §32.6 / stdlib §10.1 (v0.240) — the issue's own repro, byte for byte.
// `Length` is "byte length, excluding the null terminator" (stdlib §10.1), so
// the answer is 3.  Pre-fix: ASan heap-buffer-overflow inside `bz$M.C$len`
// (8-byte `%bz.string` field-1 read off the end of the 13-byte buffer).
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_param_length_is_strlen_spec_32_6) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Len(c: CString): int "
        "    return c.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    return C.Len(CString.From(\"abc\")); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// spec §32.6 / stdlib §10.1 — the SILENT-WRONG-VALUE face of the same defect.
// With a longer string the `%bz.string` field-1 read stays inside the (larger)
// buffer, so pre-fix there was no ASan report at all — just a wrong `Length`.
// A BOUND receiver is used here so the only thing under test is the callee's
// member read (the caller-side reclaim of (B) is not involved).
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_param_length_bound_arg_spec_32_6) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method Len(c: CString): int "
        "    return c.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var s := CString.From(\"abcdefghij\"); "
        "    return C.Len(s); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// stdlib §10.2 (v0.240): `WString.Length` is the UTF-16 code-unit count via
// bz_wstrlen.  The same annotation-shape hole, the same ASan heap-buffer-
// overflow pre-fix (measured, in `bz$M.C$wlen`).
BEZEL_TEST(codegen_ffi_temp_call_2809, wstring_param_length_spec_10_2) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method WLen(w: WString): int "
        "    return w.Length; "
        "  end WLen; "
        "  public static method Main(): int "
        "    return C.WLen(WString.From(\"abc\")); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// stdlib §10.3 (v0.240) — the RawBytes row, with an honest note on WHICH half
// of #2809 it observes.  `RawBytes` is a { ptr, len } struct whose field 1 IS
// the byte count, so the string fallback's `loadStringLen` returned the RIGHT
// NUMBER by layout coincidence: the VALUE half of this row would pass either
// way, and it is here only because the fix stops depending on that coincidence
// (the receiver now routes to bz_rawbytes_length).  The row still fails
// pre-fix, on the RECLAIM half — `RawBytes.From(b)` in argument position is an
// unbound owned temporary that leaked, and LeakSanitizer fires (measured).
BEZEL_TEST(codegen_ffi_temp_call_2809, rawbytes_param_length_spec_10_3) {
    auto r = runAotChecked(
        "module M; class C "
        "  public static method RLen(rb: RawBytes): int "
        "    return rb.Length; "
        "  end RLen; "
        "  public static method Main(): int "
        "    var b: Array<byte> := [10, 20, 30]; "
        "    return C.RLen(RawBytes.From(b)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// stdlib §10.1 (v0.240): `CString.ToBytes(): owned Array<byte>` — the same
// declaration-only guard, one file over (LlvmBackend_Calls.cpp).  On a `c:
// CString` PARAMETER receiver the arm declined, the call reached no handler,
// and codegen emitted its "initializer failed to lower … degraded to a
// zero-initialized value" internal error (#2434).
//
// (The `using var` here still draws a SPURIOUS "vacuous `using var`" §28.9
// warning on a parameter receiver — a sema-side instance of the same
// annotation-shape blindness, filed as #2826.  It is a warning: the program
// compiles, runs, and is leak-clean, which is what this row asserts.  The
// plain-`var` spelling of the same binding leaks the array — #2824 row B — so
// `using var` is both correct and required here.)
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_param_tobytes_lowers_spec_10_1) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method N(c: CString): int "
        "    using var b := c.ToBytes(); "
        "    return b.Count; "
        "  end N; "
        "  public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var s := CString.From(\"abcd\"); "
        "      t := t + C.N(s); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 80 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// stdlib §10.3 (v0.240): `RawBytes.CopyTo(destination, destOffset)` — the last
// §10 member handler with the declaration-only guard, and the worst-behaved of
// them: on a `rb: RawBytes` PARAMETER receiver the arm declined SILENTLY and
// the call became a NO-OP.  The round-trip sum came back 0 instead of 26 — a
// silent wrong answer with no diagnostic anywhere.
//
// Plain `runAot`, deliberately: `CopyTo`'s DESTINATION array local is not
// disposed at scope end (48 B + 16 B), a pre-existing leak that reproduces on
// the always-working `using var`-bound receiver spelling too — filed as #2824
// row C, measured identical before and after this commit.  Arming LSan here
// would make this row fail for that unrelated reason; the value assertion is
// what pins #2809's half.
BEZEL_TEST(codegen_ffi_temp_call_2809, rawbytes_param_copyto_copies_spec_10_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Sum(rb: RawBytes): int "
        "    var dst: Array<byte> := [0, 0, 0, 0]; "
        "    rb.CopyTo(dst, 0); "
        "    return dst[0] + dst[1] + dst[2] + dst[3]; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var src: Array<byte> := [5, 6, 7, 8]; "
        "    using var rb := RawBytes.From(src); "
        "    return C.Sum(rb); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 26);
}

// ===========================================================================
// (B) THE RECLAIM
// ===========================================================================

// spec §28.3 *Unbound owned temporaries* (v0.240): "Such a temporary is
// disposed at the end of the full expression in which it is produced, after
// every use of it within that expression has completed."  An ordinary static
// method call is the plainest instance of that sentence and leaked 41 B per
// iteration pre-fix (`bz_string_to_utf8z` <- `main`, 820 B / 20 objects).
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_temp_arg_reclaimed_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Take(c: CString): int "
        "    return c.Length; "
        "  end Take; "
        "  public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      t := t + C.Take(CString.From(\"abcdefghij\")); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 200 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §28.3 / stdlib §10.2 + §10.3: the same reclaim for the other two
// factories the #2728 table admits — `WString.From` (free) and `RawBytes.From`
// (the two-level bz_rawbytes_free: the struct AND its buffer) — plus
// `CString.FromBytes`.  RawBytes is the row that would show a HALF reclaim.
BEZEL_TEST(codegen_ffi_temp_call_2809, wstring_rawbytes_temp_args_reclaimed_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method W(w: WString): int "
        "    return w.Length; "
        "  end W; "
        "  public static method R(rb: RawBytes): int "
        "    return rb.Length; "
        "  end R; "
        "  public static method B(c: CString): int "
        "    return c.Length; "
        "  end B; "
        "  public static method Main(): int "
        "    var b: Array<byte> := [1, 2, 3, 4]; "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      t := t + C.W(WString.From(\"abc\")); "
        "      t := t + C.R(RawBytes.From(b)); "
        "      t := t + C.B(CString.FromBytes(b)); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 220 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §28.3 (v0.240): a CONSTRUCTOR argument is the same unbound owned
// temporary — `buildOrderedArgs` serves both, so this pins that the in-module
// `new` path is covered too (17 B/construction pre-fix).
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_temp_ctor_arg_reclaimed_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class W "
        "  private _n: int; "
        "  public constructor(c: CString) _n := c.Length; end constructor; "
        "  public method N(): int return _n; end N; "
        "end W; "
        "class C public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var w := new W(CString.From(\"abcd\")); "
        "      t := t + w.N(); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 80 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §28.3 (v0.240): "When the full expression is instead exited by an
// exception — an operand or argument evaluated later throws before the
// temporary's transfer or consumption point — every already-produced,
// not-yet-consumed unbound owned temporary is disposed during unwinding."
// The `catch` is INSIDE the loop, so each iteration ALSO leaves the block
// normally afterwards: that is the DOUBLE-FREE control for the reclaim's
// null-after-free (an unwind-edge free followed by a normal-edge free of the
// same slot aborts under ASan and clears `ran`).
BEZEL_TEST(codegen_ffi_temp_call_2809, cstring_temp_arg_reclaimed_on_unwind_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Boom(): int "
        "    throw new Exception(\"boom-2809\"); "
        "  end Boom; "
        "  public static method Mix(c: CString, n: int): int "
        "    return c.Length + n; "
        "  end Mix; "
        "  public static method Main(): int "
        "    var caught: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      try "
        "        var q := C.Mix(CString.From(\"abcdefghij\"), C.Boom()); "
        "      catch "
        "        var e: Exception => caught := caught + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if caught <> 20 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §32.6 / stdlib §9.6 (v0.240) — the DOUBLE-FREE control for the arm
// #2734 already covers: `string.From(CString.From(...))` registers its
// temporary in `tryLowerBuiltinClassCall`'s own arm.  If the new general-call
// registration also claimed it, the buffer would be freed twice and abort under
// ASan.  It does not: that call never reaches `buildOrderedArgs`.
BEZEL_TEST(codegen_ffi_temp_call_2809, nested_string_from_not_double_freed_spec_32_6) {
    auto r = runAotChecked(
        std::string(
        "module M; class C public static method Main(): int "
        "    var n: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      var s := string.From(CString.From(\"abc\")); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if n <> 60 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §32.6 (v0.240) — the same DOUBLE-FREE control for the EXTERN argument
// arm (#2728/#2733), which builds its own argument list and registers there.
BEZEL_TEST(codegen_ffi_temp_call_2809, extern_arg_temp_not_double_freed_spec_32_6) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "extern \"c\" public method strlen(s: CString): long; end; "
        "class C public static method Main(): int "
        "    var n: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      n := n + strlen(CString.From(\"abc\")); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if n <> 60 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §32.6 (v0.240) — the OVER-REJECTION / OVER-RECLAIM control this fix most
// needed.  stdlib §9.6 (v0.240) states of the bound source: "The source
// `CString` or `WString` retains its ownership and disposal obligation."  A
// `using var`-bound receiver passed as an argument must therefore be freed by
// its BINDING and by nothing else.  `isFfiBufferFactoryCall` matches only a
// directly-written factory call, so a bare identifier never registers — if that
// were wrong this row would double-free and abort under ASan.
BEZEL_TEST(codegen_ffi_temp_call_2809, bound_cstring_arg_freed_once_by_binding_spec_32_6) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Take(c: CString): int "
        "    return c.Length; "
        "  end Take; "
        "  public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var s := CString.From(\"abcdefghij\"); "
        "      t := t + C.Take(s) + C.Take(s); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 400 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §28.3 (v0.240) — an INTERFACE-dispatched instance method and a GENERIC
// method both route through `buildOrderedArgs`; this row pins that neither
// bypasses the reclaim and that the callee's `.Length` is right in both.
BEZEL_TEST(codegen_ffi_temp_call_2809, iface_and_generic_callee_temp_reclaimed_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "interface ILen method Of(c: CString): int; end ILen; "
        "class Impl : ILen "
        "  public constructor() end constructor; "
        "  public method Of(c: CString): int return c.Length; end Of; "
        "end Impl; "
        "class C "
        "  public static method Pair<T>(v: T, c: CString): int "
        "    return c.Length; "
        "  end Pair; "
        "  public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var im := new Impl(); "
        "      var iface: ILen := im; "
        "      t := t + iface.Of(CString.From(\"abc\")); "
        "      t := t + C.Pair<int>(7, CString.From(\"de\")); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// spec §28.3 (v0.240) — a PROPERTY setter and an INDEXER setter are argument
// positions too (the value, and the index).  Pinned because they reach the call
// machinery by a different route than a plain method call.
BEZEL_TEST(codegen_ffi_temp_call_2809, property_and_indexer_arg_temp_reclaimed_spec_28_3) {
    auto r = runAotChecked(
        std::string(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  private _m: int; "
        "  public constructor() _n := 0; _m := 0; end constructor; "
        "  public property Slot: int get => _n; set => _n := value; end; "
        "  public property [i: int]: int get => _m + i; set => _m := value; end; "
        "  public static method LenOf(c: CString): int "
        "    return c.Length; "
        "  end LenOf; "
        "end Box; "
        "class C public static method Main(): int "
        "    var t: int := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var b := new Box(); "
        "      b.Slot := Box.LenOf(CString.From(\"abcde\")); "
        "      b[Box.LenOf(CString.From(\"ab\"))] := Box.LenOf(CString.From(\"xyz\")); "
        "      t := t + b.Slot + b[0]; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 160 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

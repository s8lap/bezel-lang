// fix #2828 — unbound owned FFI buffer used as RECEIVER of a §10 member read
// (`.Length` / `.Pointer`) is never reclaimed. The pre-fix code at
// src/codegen/LlvmBackend_Exprs.cpp:3607 (the `.Length` arm) and :3659 (the
// `.Pointer` arm) called `lowerExpr(*ma->target)` and dropped the result — the
// unbound-owned buffer the receiver carried was not registered through
// `registerFfiExprTemp`, so its malloc'd buffer simply leaked.
//
// Spec citations:
//   - §28.3 *Unbound owned temporaries* (v0.240) — a member access on an
//     owned value is one of the listed BORROWING uses; the borrowed value
//     "is disposed at the end of the full expression in which it is
//     produced, after every use of it within that expression has completed".
//     A `.Length` / `.Pointer` read on `CString.From("hello")` is exactly
//     that — the read is the use, the disposal runs once the read returns.
//   - §32.5 / §32.6 — the three §10 FFI-buffer classes (`CString` /
//     `WString` / `RawBytes`) are owned, malloc-allocated buffers; the
//     disposal obligation is the programmer's on a bound spelling, the
//     language's on the unbound spelling. A receiver of a §10 member read
//     is the latter shape; the language must honor it.
//
// THE FIX. After `lowerExpr(*ma->target)` returns the buffer pointer, when
// the target expression is itself an unbound owned factory call
// (`isFfiBufferFactoryCall` — the same predicate buildOrderedArgs,
// string.From's buffer arm, and lowerCall's imported-method arm already use
// for `§28.3`'s machinery), route the result through `registerFfiExprTemp`
// so the buffer reaches `flushFfiExprTemps` at the full-expression boundary
// — where §28.3 puts the disposal. The `using var` spelling is untouched:
// `isFfiBufferFactoryCall` admits only a directly-written factory call,
// which by construction is bound to no name, so exactly one free is
// emitted for exactly one malloc.
//
// FAIL-WITHOUT, MEASURED. With the fix reverted (both arms back to plain
// `lowerExpr` + drop), under generated-code ASan + LSan:
//   - row A leaks 6 * 20 = 120 B / 20 buffers of `CString.From("hello")`;
//   - row B leaks 12 * 20 = 240 B (UTF-16 buffer is 2× the code-unit count);
//   - row C leaks the RawBytes struct (16 B * 20 = 320 B) plus the inner
//     buffer (5 * 20 = 100 B) — `isFfiBufferFactoryCall` distinguishes
//     `RawBytes` by setting `isRawBytes=true`, so the free matches the
//     struct;
//   - rows D–F are the same for `.Pointer`.
// With the fix in place every row passes.
//
// The same leak reproduces on the `.Pointer` arm; the comment on
// ffi_buffer_pointer_2825_test.cpp:row G notes it as PRE-EXISTING and
// independent of #2825 — but it is the very bug this commit fixes, so that
// row is flipped to `runAotChecked` here (the test for #2825's value is
// still pinned by rows A–K of that file).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 7 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// The churn tail every leak row shares: an allocation AFTER the measured
// loop, so no register still holds the last iteration's buffer when
// LeakSanitizer runs. A one-shot `Main` is not a reliable LSan oracle.
const char* kChurn =
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 90; end; ";

}  // namespace

// ===========================================================================
// A — `CString.From("hello").Length` (§10.1, §28.3): an unbound owned
// `CString` used as a §10 member-read RECEIVER leaks its 6-byte buffer
// (strlen("hello") = 5, plus the NUL = 6) under LSan if the receiver is
// not registered through `registerFfiExprTemp`. Value + 20-iteration leak
// oracle: pre-fix ASan/LSan reports 6 * 20 = 120 B unreclaimed.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           cstring_length_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      var n: int := CString.From(\"hello\").Length; "
        "      t := t + n; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// B — `WString.From("hello").Length` (§10.2, §28.3): the UTF-16 twin. A
// `WString` carries a `uint16_t*` buffer; bz_wstrlen walks it to the
// terminating 0x0000 code unit and returns 5. The malloc'd buffer is
// (5 + 1) * 2 = 12 B per iteration; 20 iterations = 240 B leaked pre-fix.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           wstring_length_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      var n: int := WString.From(\"hello\").Length; "
        "      t := t + n; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// C — `RawBytes.From(b).Length` (§10.3, §28.3): the `{ ptr, len }`-struct
// receiver. The factory mints a struct holding the copied buffer and its
// length; both must be reclaimed. `isFfiBufferFactoryCall` returns
// `isRawBytes=true` for `RawBytes.From`, which routes the slot through
// the RawBytes free rather than the single-pointer free used for
// CString/WString. 20 iterations × (16 B struct + 5 B inner buffer) leaks
// pre-fix.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           rawbytes_length_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var b: Array<byte> := [1, 2, 3, 4, 5]; "
        "    var t: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      var n: int := RawBytes.From(b).Length; "
        "      t := t + n; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// D — `CString.From("hello").Pointer` (§10.1, §28.3): the §32.10 ALIAS row.
// Pre-fix, the receiver leaks just as in row A — the read itself is
// `buf = lowerExpr(target)`, the buf is then `CreatePtrToInt`'d to i64
// and dropped. Value oracle: `c.Pointer` is non-null AND consumed inside
// the same full expression (read by `string.FromCStringPointer`).
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           cstring_pointer_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      var s := string.FromCStringPointer("
        "        CString.From(\"hello\").Pointer); "
        "      if s <> \"hello\" then return 1; end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// E — `WString.From("hello").Pointer` (§10.2, §28.3): the UTF-16 twin of
// row D. Round-trip through `string.FromWStringPointer` proves both the
// alias is the receiver's own buffer (non-null) and that it points at
// UTF-16 content (the round-trip succeeds).
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           wstring_pointer_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      var s := string.FromWStringPointer("
        "        WString.From(\"hello\").Pointer); "
        "      if s <> \"hello\" then return 1; end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// F — `RawBytes.From(b).Pointer` (§10.3, §28.3): the struct-receiver
// twin. The `Pointer` is the address of the inner buffer (field 0 of
// `{ ptr, len }`), not the struct itself — but the OWNER of the inner
// buffer is the struct, so the struct must be reclaimed when §28.3 puts
// the disposal. 20 iterations × struct size leak pre-fix.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           rawbytes_pointer_temp_receiver_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var b: Array<byte> := [10, 20, 30]; "
        "    var i := 0; "
        "    while i < 20 do "
        "      var p := RawBytes.From(b).Pointer; "
        "      if p = IntPtr.Zero then return 1; end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// G — CONTROL: a `using var`-bound receiver is NOT an unbound owned
// temporary (§28.3) — the `using var` itself owns the disposal. The fix
// must not double-free or interfere with that path. 20 iterations + churn
// under ASan/LSan, both for `.Length` and `.Pointer`. Passes both
// BEFORE and AFTER the fix; arms the test with an oracle sensitive to a
// false-positive fix.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           bound_receiver_not_double_freed_spec_28_3) {
    const std::string src = std::string(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t: long := 0; var i: int := 0; "
        "    while i < 20 do "
        "      using var c := CString.From(\"hello\"); "
        "      using var w := WString.From(\"hello\"); "
        "      t := t + c.Length; "
        "      if w.Pointer = IntPtr.Zero then return 1; end; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if t <> 100 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// H — VALUE ROW, pinned by `runAot`: the same `.Length` read at a single
// iteration returns 5 for `"hello"`. Passes both before and after the fix
// (the value is independent of the leak) — but it pins the §10 member
// lowering itself, which the leak rows above only test as a side effect.
// Without this row a regression that broke `.Length`'s return value but
// happened to free the buffer would still pass A–G.
// ===========================================================================
BEZEL_TEST(codegen_ffi_member_receiver_release_2828,
           cstring_length_temp_value_spec_10_1) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    return CString.From(\"hello\").Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

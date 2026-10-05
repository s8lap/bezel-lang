// Codegen — three owned-LOCAL leaks around the FFI surface (issue #2824,
// spec §10.1-§10.3 / §28.3 / §32.5-§32.6, v0.278). Each of rows A, B, C is a
// distinct surface — they share a common root only at the level of the OWN
// obligation on the local — so the regression test pins all three shapes
// independently; a future regression in any one would re-leak at a
// characteristic offset from the others (260 B / 20 allocations for row A,
// ~1040 B / 40 allocations for row B, 72 B / 2 allocations for row C) and
// surface in a different test cell.
//
// Spec (revision v0.278):
//   §10.1 "CString"           — owned CString factory + destructor.
//   §10.2 "WString"           — owned WString factory + destructor.
//   §10.3 "RawBytes"          — owned RawBytes + CopyTo(Array<byte>, int).
//   §28.3 "Local variables, ownership, and borrows" — auto-drop /
//     auto-dispose; the local owns the malloc'd buffer, the scope-end
//     reclaims it.
//   §32.5 "FFI-compatible types" / §32.6 "String marshalling" — the
//     caller-side scope-end release contract.
//
// Shape coverage — every leak surface the issue enumerates:
//   T1 (row A) a plain `var c := CString.From("abc")` in a loop.
//       Allocates 1 CString per iteration; pre-fix the local is classified
//       Borrowed (the FFI-buffer return type lacks a user `declaration` so
//       `isClassOrInterfaceResult` returns false) and `autoDropAtScopeEnd`
//       is never set, so the malloc'd buffer leaks.
//   T2 (row B) a plain `var b := c.ToBytes()` (returning `owned Array<byte>`)
//       in a loop. Same root: the call's result type is the heap-pointer
//       builtin Array<T>, also declaration-less; same Borrowed verdict,
//       same leak.
//   T3 (row B sibling) `WString.From("...")` — the WString half of the
//       FFI-buffer trio, fixed in lockstep with CString and RawBytes.
//   T4 (row C) `var dst: Array<byte> := [0, 0, 0, 0]; rb.CopyTo(dst, 0);`.
//       The borrowing-Array walk's StringEscapeWalker taints `dst` as
//       ESCAPE because the FFI-buffer instance-method call (CopyTo) does
//       not resolve to a `MethodSymbol` (`ctx.exprSymbol(callee)` returns
//       null for an imported builtin instance method on an imported
//       builtin class), so the existing ms-gated call-bounded-borrow
//       relaxation never fires and the local's scope-end free is suppressed.
//   T5 (row C ASan oracle) exitCode == 0 under `runAotChecked` is the
//       LSan-leak assertion; the leaky pre-fix build exits 1.

// Header is included for documentation; the test lives under tests/codegen/.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// ===========================================================================
// T1 (row A) — plain `var c := CString.From("abc")` in a 20-iteration loop.
// pre-fix leaked 260 byte(s) in 20 allocations from bz_string_to_utf8z.
// ===========================================================================
const char* kPlainCStringFrom =
    "module M2824A; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 20 do "
    "      var c := CString.From(\"abc\"); "
    "      if c.Length <> 3 then return 3; end; "
    "      i := i + 1; "
    "    end; "
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 2; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ===========================================================================
// T2 (row B) — plain `var b := c.ToBytes()` in a 20-iteration loop.
// pre-fix leaked 1040 byte(s) in 40 allocations (Array<byte> backing + struct).
// ===========================================================================
const char* kPlainCStringToBytes =
    "module M2824B; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    var c: CString := CString.From(\"xyz\"); "
    "    while i < 20 do "
    "      var b := c.ToBytes(); "
    "      if b.Count <> 3 then return 3; end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ===========================================================================
// T3 (row B WString sibling) — WString.From(s) over 20 iterations.
// Same root as row A; the WString half of the FFI-buffer trio.
// ===========================================================================
const char* kPlainWStringFrom =
    "module M2824W; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 20 do "
    "      var w := WString.From(\"hi\"); "
    "      if w.Length <> 2 then return 3; end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ===========================================================================
// T4 (row C) — `var dst: Array<byte> := [...]; rb.CopyTo(dst, 0);` is the
// borrowing-Array walk's StringEscapeWalker escape path. pre-fix leaked
// 56 B in 1 object (the dst array struct) + 16 B in 1 object (the
// backing buffer) — 72 B / 2 allocations. The plain value-26 oracle
// proves the right bytes were copied, so the leak is purely a scope-end
// free missing.
// ===========================================================================
const char* kRawBytesCopyTo =
    "module M2824C; "
    "class C "
    "  public static method Main(): int "
    "    var src: Array<byte> := [5, 6, 7, 8]; "
    "    using var rb := RawBytes.From(src); "
    "    var dst: Array<byte> := [0, 0, 0, 0]; "
    "    rb.CopyTo(dst, 0); "
    "    return dst[0] + dst[1] + dst[2] + dst[3]; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// T1 — the row-A primary. The runAot pass asserts plain (no ASan) correctness;
// runAotChecked forces generated-code ASan+LSan, and LSan forces exit 1 on any
// leak. exitCode == 0 IS the leak assertion.
// ===========================================================================
BEZEL_TEST(codegen_ffi_local_cstring_wstring_leak_2824,
           plain_var_cstring_from_is_reclaimed_spec_28_3) {
    JitResult plain = runAot(kPlainCStringFrom);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kPlainCStringFrom);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports 260 B / 20)
}

// ===========================================================================
// T2 — the row-B primary. Same oracle.
// ===========================================================================
BEZEL_TEST(codegen_ffi_local_cstring_wstring_leak_2824,
           plain_var_cstring_to_bytes_is_reclaimed_spec_28_3) {
    JitResult plain = runAot(kPlainCStringToBytes);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kPlainCStringToBytes);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1 (LSan reports ~1040 B / 40)
}

// ===========================================================================
// T3 — the row-B WString sibling. Same oracle.
// ===========================================================================
BEZEL_TEST(codegen_ffi_local_cstring_wstring_leak_2824,
           plain_var_wstring_from_is_reclaimed_spec_28_3) {
    JitResult plain = runAot(kPlainWStringFrom);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    JitResult r = runAotChecked(kPlainWStringFrom);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: 1
}

// ===========================================================================
// T4 — the row-C primary. The plain value-26 oracle (dst[0]+...+dst[3]==26)
// proves the right bytes were copied, so the leak is purely a scope-end
// free missing on the dst array local.
// ===========================================================================
BEZEL_TEST(codegen_ffi_local_cstring_wstring_leak_2824,
           rawbytes_copyto_destination_array_is_reclaimed_spec_10_3) {
    JitResult plain = runAot(kRawBytesCopyTo);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 26);   // 5+6+7+8 = 26
    JitResult r = runAotChecked(kRawBytesCopyTo);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 26);   // pre-fix: 1 (LSan reports 72 B / 2)
}

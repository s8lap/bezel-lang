// fix #1706 — spec §32.5 *Boundary `out` and `ref`* (v0.306).
//
// THE RULE. "`ref p: T` — C sees `T*`, receiving the **address of the caller's
// storage**. Direct passing is the *normative* semantics, not an as-if
// shortcut"; "`out p: T` — same address passing; the caller's recipient need
// not be initialized", and "after the call returns, the argument variable is
// definitely assigned".
//
// THE DEFECT. The extern call path (`lowerExternCall`, LlvmBackend_Calls2.cpp)
// lowered EVERY argument with `lowerExpr` — i.e. by VALUE — and `declareExterns`
// gave a `ref`/`out` slot the lowered VALUE type. So a boundary `ref`/`out`
// argument passed the caller's *current contents* where C expected a `T*`, and
// the native callee's write landed nowhere. No diagnostic fired anywhere: the
// program compiled, ran, and silently kept the recipient's pre-call value. The
// declarations themselves were already accepted (`s32_5_0014` in the rule
// corpus is a banked `accept`), so the whole defect was invisible to any
// declaration-only oracle — only a call that READS BACK the out-value shows it.
//
// WHY THESE LIBC FUNCTIONS. Each writes through its pointer parameter and also
// returns a value, so a single call pins both halves: the return value proves
// the call itself was well-formed (a wrong ABI here would corrupt it), and the
// written-back recipient proves the address crossed. The expected values are
// exact and standard:
//   strtol("42rest", &end, 10)  → 42, and `end` points at "rest" (non-NULL).
//   modf(3.75, &ip)             → 0.75, and `ip` == 3.0.          (`ref`)
//   frexp(8.0, &e)              → 0.5,  and `e`  == 4.            (`out var`)
// FAIL-WITHOUT: pre-fix every recipient keeps its pre-call value (IntPtr.Zero /
// 0.0 / 0) while the RETURN value is still correct — which is why each row
// asserts the recipient, not just the return.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §32.5 *Boundary `out` and `ref`*: `out p: IntPtr` — C sees `IntPtr*`.
// strtol's `char **endptr` is the canonical shape ("declare `out p: IntPtr`
// and read/wrap explicitly at the use site", §32.5's `ref`/`out`-on-CString
// exclusion). Pre-fix: `end` stays IntPtr.Zero, so the program prints
// "v=42 endSet=0".
BEZEL_TEST(codegen_ffi_boundary_outref,
           out_intptr_receives_native_write_spec_32_5_1706) {
    auto r = runAot(
        "module M; "
        "extern \"c\" method strtol(nptr: CString, out endptr: IntPtr, "
        "                           radix: int): long; end; "
        "class C public static method Main(): int "
        "  using var s := CString.From(\"42rest\"); "
        "  var e: IntPtr := IntPtr.Zero; "
        "  var v := strtol(s, out e, 10); "
        "  var endSet := 0; "
        "  if e <> IntPtr.Zero then endSet := 1; end; "
        "  Console.WriteLine($\"v={v} endSet={endSet}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v=42 endSet=1\n"));
}

// §32.5 *Boundary `out` and `ref`*: `ref p: double` — C sees `double*` and the
// caller's storage is written in place. modf(3.75) splits into 0.75 + 3.0.
// Pre-fix: `ip` keeps its initializer 0.0 → "frac=0.75 ip=0".
BEZEL_TEST(codegen_ffi_boundary_outref,
           ref_double_receives_native_write_spec_32_5_1706) {
    auto r = runAot(
        "module M; "
        "extern \"c\" method modf(x: double, ref iptr: double): double; end; "
        "class C public static method Main(): int "
        "  var ip: double := 0.0; "
        "  var frac := modf(3.75, ref ip); "
        "  Console.WriteLine($\"frac={frac} ip={ip}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("frac=0.75 ip=3\n"));
}

// §32.5 *Boundary `out` and `ref`*: the §22.4 inline declaration `out var x`
// "is admitted" at a boundary `out` parameter, and "after the call returns,
// the argument variable is definitely assigned". frexp(8.0) = 0.5 * 2^4.
// Pre-fix: the inline recipient keeps its zero-init → "mant=0.5 e=0".
BEZEL_TEST(codegen_ffi_boundary_outref,
           out_var_inline_declaration_receives_native_write_spec_32_5_1706) {
    auto r = runAot(
        "module M; "
        "extern \"c\" method frexp(x: double, out exp: int): double; end; "
        "class C public static method Main(): int "
        "  var mant := frexp(8.0, out var e); "
        "  Console.WriteLine($\"mant={mant} e={e}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("mant=0.5 e=4\n"));
}

// §32.5 *Boundary `out` and `ref`*: "for an **accessor-mediated** target the
// §22.3 copy-in/copy-out bracketing applies unchanged (the hidden slot's
// address is what crosses; write-back on return)". A property target has no
// true-alias location, so `lvalueAddr` returns null and the bracket is what
// carries the value back. Pre-fix the whole by-address branch was absent, so
// the property kept its pre-call value → "e=0 seen=0".
BEZEL_TEST(codegen_ffi_boundary_outref,
           out_accessor_mediated_property_write_back_spec_32_5_1706) {
    auto r = runAot(
        "module M; "
        "extern \"c\" method frexp(x: double, out exp: int): double; end; "
        "public class Holder "
        "  private _e: int := 0; "
        "  private _writes: int := 0; "
        "  public constructor() end constructor; "
        "  public property Slot: int "
        "    get => _e; "
        "    set => do _e := value; _writes := _writes + 1; end; "
        "  end Slot; "
        "  public property Writes: int => _writes; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var mant := frexp(8.0, out h.Slot); "
        "  Console.WriteLine($\"mant={mant} e={h.Slot} seen={h.Writes}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("mant=0.5 e=4 seen=1\n"));
}

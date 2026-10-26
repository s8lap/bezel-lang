// Regression test for #2823 — §32.5 (FFI parameters), §32.6 (FFI string
// marshalling), §28.3 (auto-drop / exception-path-discharge), §28.9
// (Runtime-managed types), v0.273.
//
// THE BUG. `registerUnwindOwnedParam` (src/codegen/LlvmBackend_Setup.cpp:16026)
// registered auto-drop disposals for owned parameters of CLASS/INTERFACE types
// and for the compiler-known builtin endpoints ChannelSender<T> / Channel<T> /
// Shared<T> (the #1957 sibling branch at line ~16071) and the collection family
// OwningArray<T> / Array<T> / OwningDictionary<K,V> / LinkedList<T> / Set<T> /
// Stack<T> / Queue<T> (the #3033 sibling branch at line ~16160). The FFI buffer
// trio CString / WString / RawBytes has NEITHER a user TypeSymbol NOR a builtin
// class type — they are plain malloc'd byte buffers (or, for RawBytes, a
// { ptr, len } struct whose `ptr` owns a byte buffer, §32.5/§10.3) — so they
// fall through `isClassOrInterfaceRef(rt)` at line ~16156 and the
// `Disposal{slot, nullptr}` registration was SKIPPED ENTIRELY. An `owned
// c: CString` (or WString / RawBytes) parameter that is never discharged on the
// normal path (auto-drop, or a throw that unwinds past it) leaks the buffer
// — and the §28.3 "auto-drop" diagnostic fires at exactly that call site, so
// the compiler PROMISED a release that never ran.
//
// `using var`-bound CString / WString / RawBytes (a sibling bug from #977 /
// #985) was always fine because `registerUsingDisposal` (Setup.cpp ~14513) has
// its own `ffiBuffer` arm that registers the same Disposal{slot, nullptr,
// ffiBuffer=true, rawBytes=isRaw} the parameter branch was missing. The same
// arm in `emitDisposals` (Disposal.cpp ~5285) was already null-guarded and
// null-after-free (#2733); the only missing piece was the registration.
//
// THE FIX.
//   1. `registerUnwindOwnedParam` now detects the FFI buffer family BEFORE the
//      `isClassOrInterfaceRef` gate via `isFfiBufferTypeRef(rt)` — the
//      sanctioned accessor (#2809/#2757's one home for {CString, WString,
//      RawBytes}, reading `builtinTypeName` so it answers correctly for both
//      the declaration-less annotation TypeRef and the `builtinClassSym`
//      factory-result TypeRef — same shape #2825/#2823's fix rides on) — and
//      registers a `Disposal{slot, nullptr}` with `d.ffiBuffer = true` /
//      `d.rawBytes = isRaw`, exactly mirroring the ChannelSender/Channel/Shared
//      branch (#1957) and the collection branch (#3033) one family out.
//      `releaseOnUnwindOnly = !ps->autoDropAtScopeEnd` — auto-dropped owners
//      release on both exits (the sole release); discharged owners release
//      only on the unwind edge (the normal-path discharge site is the sole
//      release on that edge).
//   2. `emitDisposals`'s `ffiBuffer` arm at Disposal.cpp ~5285 gets the same
//      `releaseOnUnwindOnly && !dropUndischarged` skip gate the collection /
//      dict / channelSender / channel / shared / mutex / lock / semaphore /
//      semaphoreToken arms already carry (#3036 / #2869). Without it, an
//      `owned c: CString` parameter explicitly Dispose()'d on the normal path
//      would dispose again on its normal-path scope-end (a genuine
//      double-free of a malloc'd buffer, which is NOT idempotent).
//
// OBSERVABILITY. The leak rows run under `runAotChecked` (AOT + generated-code
// ASan with LeakSanitizer armed). WITHOUT the fix the parameter's malloc'd
// buffer leaks at every call site; LSan flips `r.ran` to false. WITH the fix
// the buffer is freed at method exit (auto-drop case) or on the unwind edge
// (discharged-but-throwing case); `r.ran` is true and the program returns the
// expected sentinel. The functional rows use plain `runAot` and prove the
// buffer is ALIASED (the read is observable) — that the fix didn't free too
// early. The double-release rows guard the new `releaseOnUnwindOnly` gate:
// explicit `Dispose()` then throw must release exactly once.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// The churn tail every leak row shares: an allocation AFTER the measured loop,
// so no register still holds the last iteration's buffer when LeakSanitizer
// runs. A one-shot `Main` is not a reliable LSan oracle. Mirrors the pattern
// ffi_buffer_pointer_2825_test.cpp uses for the same reason.
const char* kChurn =
    "    var junk: Array<int> := [1, 2, 3]; "
    "    if junk[0] <> 1 then return 90; end; ";

}  // namespace

// ===========================================================================
// A — #2823's own repro shape for CString: an `owned c: CString` parameter
// that is NEVER explicitly discharged (auto-drop case) — §28.3 must release
// the malloc'd UTF-8 buffer at method end. WITHOUT the fix the buffer leaks
// (LSan flips r.ran false). WITH the fix the buffer is freed exactly once
// and the program returns the sentinel.
//
// Per spec §28.3 (auto-drop) / §32.5 (FFI parameters) / §32.6 (FFI string
// marshalling), stdlib §10.1 (CString), v0.273.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_param_disposal_2823,
           owned_cstring_param_autodrop_no_leak_spec_28_3_32_5_10_1) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Sink(owned c: CString) "
        "  end Sink; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      Sink(CString.From(\"hello\")); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);             // no leak (no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// B — same shape for WString. §10.2 WString is a malloc'd UTF-16 buffer
// (bz_string_to_utf16z); §28.3 auto-drop on an `owned w: WString` parameter
// must release it at method end.
//
// Per spec §28.3 / §32.5 / §32.6, stdlib §10.2, v0.273.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_param_disposal_2823,
           owned_wstring_param_autodrop_no_leak_spec_28_3_32_5_10_2) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Sink(owned w: WString) "
        "  end Sink; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      Sink(WString.From(\"hello\")); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// C — same shape for RawBytes. §10.3 RawBytes is a { ptr, len } struct whose
// `ptr` owns a malloc'd byte buffer — released via bz_rawbytes_free, which
// frees the buffer THEN the struct, exactly once (fix #985). §28.3 auto-drop
// on an `owned b: RawBytes` parameter must free the buffer AND the struct.
//
// Per spec §28.3 / §32.5, stdlib §10.3, v0.273.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_param_disposal_2823,
           owned_rawbytes_param_autodrop_no_leak_spec_28_3_32_5_10_3) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Sink(owned b: RawBytes) "
        "  end Sink; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 20 do "
        "      Sink(RawBytes.From([1, 2, 3, 4])); "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// D — FUNCTIONAL ROW (plain runAot, no LSan). The fix must not free the
// buffer TOO EARLY: an `owned c: CString` parameter must be a live buffer
// for the whole body of Sink — strlen() through libc reads it, and the
// returned length (5 for "hello") is the proof. Pre-fix this row passes
// too (the buffer was leaked, not freed early). Post-fix it still passes,
// which is exactly the property: the disposal is at method end, not at
// parameter receipt.
//
// Per spec §28.3 (auto-drop fires at method end) / §32.6 (FFI string
// marshalling, the parameter is borrowed by C during the call), stdlib
// §10.1, v0.273.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_param_disposal_2823,
           owned_cstring_param_autodrop_buffer_live_for_call_spec_32_6) {
    auto r = runAot(
        std::string(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C "
        "  public static method Sink(owned c: CString): int "
        "    return strlen(c); "
        "  end Sink; "
        "  public static method Main(): int "
        "    return Sink(CString.From(\"hello\")); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);       // strlen("hello") == 5
}

// ===========================================================================
// E — BLOCKER regression (the new releaseOnUnwindOnly gate). An `owned
// c: CString` parameter never discharged (auto-drop case) on the throw
// unwind edge. §28.3's exception-path disposal must fire (the parameter
// holds a live value at the throw site by construction — it was never
// moved out, returned, or reassigned). CString has NO `Dispose()` method
// (it's a builtin FFI buffer, freed only by §28.3 auto-drop or the
// releaseOnUnwindOnly unwind), so the only "single release" semantics
// available here IS the throw-edge free. WITHOUT the fix the buffer
// leaks at every iteration (LSan flips r.ran to false); WITH the fix
// the registered Disposal fires on the unwind edge — the same
// releaseOnUnwindOnly=false auto-drop path the normal-exit case
// (test A) uses, just on the OTHER edge.
//
// Per spec §28.3 (exception-path-discharge) / §32.5, stdlib §10.1, v0.273.
// ===========================================================================
BEZEL_TEST(owned_ffi_buffer_param_disposal_2823,
           owned_cstring_param_unwind_autodrop_no_leak_spec_28_3_32_5) {
    auto r = runAotChecked(
        std::string(
        "module M; class C "
        "  public static method Sink(owned c: CString) "
        "    throw new Exception(\"boom\"); "
        "  end Sink; "
        "  public static method Main(): int "
        "    var i := 0; var caught := 0; "
        "    while i < 20 do "
        "      try "
        "        Sink(CString.From(\"hello\")); "
        "      catch Exception => do "
        "        caught := caught + 1; "
        "      end "
        "      end try; "
        "      i := i + 1; "
        "    end; ") + kChurn +
        "    if caught <> 20 then return 91; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);             // no leak — single release on unwind edge
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

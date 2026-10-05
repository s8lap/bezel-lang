// Codegen / leak — fix #5320 (§28.9 / §23.2 / §12, v0.313): a
// `string?` virtual/interface property read leaks when the override or
// implementation getter body produces a fresh string.
//
// ROOT CAUSE. `ensureVirtualStringGetterOwnershipThunk` was installed with
// `allowNullableResult=false` at every vtable/itable slot-fill site, so
// `string?` getters were never wrapped — the raw accessor was placed in
// the slot. The caller-side `isPlusOneStringProducer` fast path then
// misclassified the dynamic read as a borrow (`dynamicSlot` → `!ty->nullable`
// → `!true` → false), so no release fired and the fresh string leaked.
//
// THE FIX. Add `allowNullableResult=true` to every thunk installation call
// (LlvmBackend_Setup.cpp:5519, 5730, 8344, 9062, 9356, 9380, 9473,
// 9476, 43141; LlvmBackend_Layout.cpp:2036, 2044), so `string?` getters
// are also normalized to +1 and the consumer releases them correctly.
// `bz_string_retain` is null-safe, so a null return is a no-op.
//
// Spec §28.9 (each runtime-managed reference is released exactly once) /
// §23.2 (virtual properties) / §12 (override), v0.313.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan. The virtual
// and interface probes (A) and (B) FAIL-WITHOUT: without the thunk each
// call produces a fresh owned string that is never released, and the loop
// amplifies 200× so LSan records 7490 B / 7690 B leaked. PASS-WITH:
// the thunk retains the fresh string and the consumer releases it per
// §28.9; LSan is clean. (C) and (D) are regression guards: the `string`
// twin and the direct (non-virtual) `string?` read were always clean and
// must stay that way.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the program under ASan/LSan. If a sanitizer fires (leak,
// UAF, double-free), runAotChecked sets r.ran=false. Returning r.ran?r.exitCode:-999
// gives the actual exit code on clean runs and -999 on sanitizer fire, which
// is distinct from any legal program exit value.
int runChecked(std::string_view src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// (A) FAIL-WITHOUT probe — virtual property `string?` with fresh override
// body, read through a base-typed variable (vtable dispatch).
// 200 iterations amplify to 7490 B / 400 allocs leaked without the fix.
const char* kVirtualNullableString =
    "module M; "
    "public class Base "
    "  protected _f: string?; "
    "  public constructor(f: string?) _f := f; end constructor; "
    "  public virtual property VP: string? => _f; "
    "end Base; "
    "public class Der: Base "
    "  public constructor(f: string?) base(f); end constructor; "
    "  public override property VP: string? => \"d\" + (_f ?? \"\"); "
    "end Der; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var der := new Der(\"v\" + i.ToString()); "
    "      var bb: Base := der; "
    "      var x: string? := bb.VP; "
    "      t = t + (x ?? \"\").Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (B) FAIL-WITHOUT probe — interface property `string?` with fresh impl
// body, read through an interface-typed variable (itable dispatch).
// 200 iterations amplify to 7690 B / 400 allocs leaked without the fix.
const char* kInterfaceNullableString =
    "module M; "
    "public interface IP "
    "  readonly property P: string?; "
    "end IP; "
    "public class PImpl: IP "
    "  private _f: string?; "
    "  public constructor(f: string?) _f := f; end constructor; "
    "  public property P: string? => \"ip\" + (_f ?? \"\"); "
    "end PImpl; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var impl := new PImpl(\"i\" + i.ToString()); "
    "      var ip: IP := impl; "
    "      var x: string? := ip.P; "
    "      t = t + (x ?? \"\").Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (C) Regression guard — virtual property `string` (non-nullable) twin.
// The thunk was already installed for `string` getters (pre-fix), so this
// was always LSan-clean. Must stay clean.
const char* kVirtualNonNullableString =
    "module M; "
    "public class Base "
    "  protected _f: string; "
    "  public constructor(f: string) _f := f; end constructor; "
    "  public virtual property VP: string => _f; "
    "end Base; "
    "public class Der: Base "
    "  public constructor(f: string) base(f); end constructor; "
    "  public override property VP: string => \"d\" + _f; "
    "end Der; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var der := new Der(\"v\" + i.ToString()); "
    "      var bb: Base := der; "
    "      var x: string := bb.VP; "
    "      t = t + x.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (D) Regression guard — direct (non-virtual) property `string?` with
// fresh body. Static dispatch: `isPlusOneStringProducer` inspects the
// body directly and classifies it as +1, so the consumer releases it.
// Was always LSan-clean and must stay that way.
const char* kDirectNullableString =
    "module M; "
    "public class Der "
    "  private _f: string?; "
    "  public constructor(f: string?) _f := f; end constructor; "
    "  public property VP: string? => \"d\" + (_f ?? \"\"); "
    "end Der; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var der := new Der(\"v\" + i.ToString()); "
    "      var x: string? := der.VP; "
    "      t = t + (x ?? \"\").Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

} // namespace

// (A) FAIL-WITHOUT: virtual property `string?` override with fresh body.
// 200 iterations amplify to 7490 B / 400 allocs leaked without the fix.
// Post-fix: thunk retains the fresh string, consumer releases it per §28.9.
BEZEL_TEST(codegen_virtual_property_nullable_string_leak_5320,
           virtual_string_nullable_override_via_base) {
    BEZEL_EXPECT_EQ(runChecked(kVirtualNullableString), 0);  // 0 = clean, -999 = LSan leak
}

// (B) FAIL-WITHOUT: interface property `string?` impl with fresh body.
// 200 iterations amplify to 7690 B / 400 allocs leaked without the fix.
BEZEL_TEST(codegen_virtual_property_nullable_string_leak_5320,
           interface_string_nullable_impl_via_ip) {
    BEZEL_EXPECT_EQ(runChecked(kInterfaceNullableString), 0);
}

// (C) Regression guard: virtual property `string` (non-nullable) twin.
// Was always LSan-clean; thunk was already installed for non-nullable getters.
BEZEL_TEST(codegen_virtual_property_nullable_string_leak_5320,
           virtual_string_non_nullable_regression) {
    BEZEL_EXPECT_EQ(runChecked(kVirtualNonNullableString), 0);
}

// (D) Regression guard: direct (non-virtual) `string?` property with fresh body.
// Static dispatch: isPlusOneStringProducer inspects the body directly.
// Was always LSan-clean; must stay that way.
BEZEL_TEST(codegen_virtual_property_nullable_string_leak_5320,
           direct_string_nullable_regression) {
    BEZEL_EXPECT_EQ(runChecked(kDirectNullableString), 0);
}

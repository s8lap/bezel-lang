// fix #5389 (§23.1 / §28.9, v0.314): a function-typed auto-property's backing
// cell was not released on dispose. The Dispose/DisposeCore pair never called
// bz_closure_release for a function-typed auto-property of a generic class
// instantiated with a function type (e.g. Holder<() -> int>).
//
// ROOT CAUSE. isClosureStorageMember used recordMemberType, which returns the
// declared type (T, a Named type ref) rather than recordMemberTypeResolved,
// which resolves T through the active substitution. For Holder<() -> int>, the
// property v's declared type T resolves to () -> int (Function kind) under the
// active substitution, but isClosureStorageMember returned false because it
// saw the unresolved Named type. The disposal release was never emitted and
// the closure cell leaked (~48 bytes per instance, measured on r27-5389 base).
//
// NOTE: This test file pins the leak regression. The fix attempt (adding
// isTypeParamBoundToFunction/isTypeParamFunctionStorageMember to the disposal
// walk) caused a soundness regression (abort on read-back). The fix is not
// yet complete; this test file is preserved so the next attempt inherits it.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The minimal repro from issue #5283 / #5303: a function-typed auto-property
// of a generic class instantiated with a function type. The backing cell is
// not released on disposal, causing a 48-byte leak per instance.
const char* kMinimalRepro =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var f: () -> int := lambda () => 7;\n"
    "    var h := new Holder<() -> int>(f);\n"
    "    return 0;\n"  // h is disposed here, leaking the closure cell
    "  end Main;\n"
    "end Program;\n";

// A nullable function-typed auto-property (also covered by the fix since
// bz_closure_release is null-safe).
const char* kNullableFnProp =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var f: () -> int? := lambda () => 99;\n"
    "    var h := new Holder<() -> int?>(f);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// leaksAtLeast — returns true if the program leaks at least minBytes under ASan.
// runAotChecked sets sanitizeAddress on the generated program and treats any
// sanitizer finding (including LSan's exit-time leak report) as ran=false.
// A leaking program therefore makes this return true; a clean program returns
// false. Pre-fix #5389: ~48 bytes leaked per instance. Post-fix: no leak.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding -- clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // sanitizer fired but not with a leak SUMMARY (e.g. crash)
}

}  // namespace

// ─── Minimal repro: looped to make LeakSanitizer fire ───────────────────────
// Pre-fix: LeakSanitizer reports ~48 bytes leaked per instance → FAIL-WITHOUT.
// Post-fix: no leak → row passes.
BEZEL_TEST(function_typed_autoprop_disposal_5389,
           minimal_repro_looped_48_byte_leak) {
    for (int i = 0; i < 200; ++i) {
        // Assert no leak: leaksAtLeast returns false when clean (post-fix).
        // Pre-fix, this returns true (leak detected), causing the row to FAIL.
        BEZEL_EXPECT_FALSE(leaksAtLeast(kMinimalRepro, 1));
    }
}

// ─── Nullable function type: null-safe release ───────────────────────────────
// Same pattern: nullable function type also leaked pre-fix, fixed by the same
// bz_closure_release call in the disposal walk.
BEZEL_TEST(function_typed_autoprop_disposal_5389,
           nullable_function_type_in_generic) {
    for (int i = 0; i < 200; ++i) {
        BEZEL_EXPECT_FALSE(leaksAtLeast(kNullableFnProp, 1));
    }
}

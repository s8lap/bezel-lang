// Codegen — issue #4342 / spec §9.7 + §28.3 + §28.9, v0.298.
//
// REGRESSION PIN for #4322's regression: when a record/choice factory
// call's result is DISCARDED (bare `Helper.Wrap(...);`) or NEVER BOUND
// to a local (inline `... + Helper.Wrap(...).V.Length`), nothing claims
// the +1 producer argument's release. `fix #4322`'s call-wide
// suppression in `lowerCallDispatch`'s #429 release loop was keyed only
// on the callee's return type shape, not on whether anything downstream
// will reclaim the argument — so the suppression leaked for the
// discarded/inline shapes (a regression from the pre-#4322 unconditional
// release). The fix scopes the suppression to calls whose result IS
// the initializer of a record-typed LocalDecl/LocalConstDecl in
// `recordCallResultReleaseEligible_`. See #4342 for full rationale.

#include <regex>
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// leaksAtLeast — measured, margined leak-magnitude floor oracle (§30).
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;
}

}  // namespace

// PRIMARY PIN — issue's repro A (discarded call result, 500 iters).
// FAIL-WITHOUT: LeakSanitizer 20390 B / 1000 allocations. PASS-WITH:
// exit 50, no ASan/LSan output.
BEZEL_TEST(record_factory_call_wide_release_suppression_scope_4342,
           discarded_call_result_no_longer_leaks) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      Helper.Wrap(\"he\" + \"llo\" + i.ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 50; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// COUNTERPART — discarded call result with a SECOND +1-producer
// argument the callee does not embed. The fix #4342 restores the
// unconditional release for discarded results, so BOTH arguments are
// reclaimed: fully clean. Pins the "discarded result reclaims every
// +1 argument" half of the fix.
BEZEL_TEST(record_factory_call_wide_release_suppression_scope_4342,
           discarded_call_result_with_unrelated_arg_fully_clean) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string, unused: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      Helper.Wrap(\"he\" + \"llo\" + i.ToString(), "
        "                  \"junk\" + i.ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 50; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// NEGATIVE CONTROL — `var outer := ...` shape from #4322's headline.
// MUST still be fully clean (no UAF, no leak). The new flag arms for
// this shape (the call IS the init), so the call-wide suppression
// stays armed and the LocalDecl's scope-end `recordStringFields
// Disposal` reclaims the argument. Pins the no-regression half.
BEZEL_TEST(record_factory_call_wide_release_suppression_scope_4342,
           bound_call_result_still_clean_no_regression) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var outer := Helper.Wrap(\"he\" + \"llo\" + i.ToString()); "
        "      total := total + outer.V.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// NEGATIVE CONTROL — `var outer := ...` with a SECOND +1-producer
// argument the callee does not embed. The call-wide suppression stays
// armed (call IS the init), so the unrelated argument's release is
// still withheld — the pre-existing leak-over-UAF safe default, not
// a #4342 regression. Floor 15000 B below measured 19890 B / 1000
// allocs (§30: floor, not exact count).
BEZEL_TEST(record_factory_call_wide_release_suppression_scope_4342,
           bound_call_result_unrelated_arg_still_leaks_call_wide) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string, unused: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var outer := Helper.Wrap(\"he\" + \"llo\" + i.ToString(), "
        "                                \"junk\" + i.ToString()); "
        "      total := total + outer.V.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 15000));
}

}  // namespace bezel::codegen

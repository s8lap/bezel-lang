// fix #5421 (§5 / §28.3, v0.303): bz_lazy_to_dict must release key/value
// closures on the duplicate-key throw path. The caller's codegen releases them
// after a successful return; on the throw path bz_lazy_to_dict never returns so
// those releases were skipped. 200 iterations of a throw-inducing call leak
// 9600 bytes (200 x 48-byte bz_closure_t).
//
// The existing `throw_path_is_lsan_clean_4_5` row is VACUOUS without
// BEZEL_AOT_DETECT_LEAKS=strict: runAotChecked defaults to detect_leaks=0,
// so the program exits cleanly and r.exitCode==0 regardless of whether
// closures were leaked. This row enables the leak oracle explicitly.
//
// PRE-FIX: LSan fires, 9600 byte(s) leaked in 200 allocation(s) → r.exitCode=-1
// POST-FIX: no leak → r.exitCode=0, test passes.
//
// stdlib §5 (ToDictionary raises ArgumentException on duplicate key),
// §28.3 / §28.9 (inline-lambda closure ownership).

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Plain program wrapper — no resource class.
std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row. Required, not tidiness:
// the gate may run a whole shard with the knob set, and a row that asserts
// knob-OFF behaviour specifically must not be polluted.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// The harness SKIPs (loudly, counted) when clang is absent; a row that asserts
// through that would be asserting on an unarmed oracle.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // A toolchain skip and a real finding are DIFFERENT outcomes, and only
        // the former may be returned past. "runAotChecked: SKIP — C toolchain
        // (clang) ..." means the sanitizer never ran; "sanitizer fired" means it
        // ran and found something. Returning true for the second would discard
        // a genuine leak as if the box merely lacked clang — the #3733 silent
        // -skip defect, and the reason this predicate is not a bare find("SKIP").
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §5 — LSan-sensitive leak pin for the duplicate-key throw path.
// 200 iterations of a throw-inducing ToDictionary call.
//
// PRE-FIX:  48 bytes/iteration x 200 = 9600 bytes leaked → LSan fires → FAIL
// POST-FIX: closures released on throw path → no leak → PASS
// ---------------------------------------------------------------------------

// fix #5421: the duplicate-key throw path must not leak closures.
// Generic-key path (int keys, generic dispatch).
BEZEL_TEST(todictionary_duplicate_key_leak_5421,
           generic_key_throw_path_no_closure_leak) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var i := 0; "
        "while i < 200 do "
        "  try "
        "    var d := arr.ToDictionary(lambda(x: int) => x); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "return 0; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    // With strict leak detection, LSan fires if any leak occurred.
    // The fix releases closures on the throw path so no leak accumulates.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #5421: the duplicate-key throw path must not leak closures.
// String-key path (string keys, string-interned dispatch).
BEZEL_TEST(todictionary_duplicate_key_leak_5421,
           string_key_throw_path_no_closure_leak) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(prog(
        "var arr: Array<string> := [\"a\", \"b\", \"a\"]; "
        "var i := 0; "
        "while i < 200 do "
        "  try "
        "    var d := arr.ToDictionary(lambda(s: string) => s); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "return 0; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    // String-key path uses a separate code path in bz_lazy_to_dict.
    // Both the generic-key and string-key throw paths must release closures.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #5421: two-selector overload duplicate-key throw path (int keys).
BEZEL_TEST(todictionary_duplicate_key_leak_5421,
           two_selector_generic_key_throw_path_no_closure_leak) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(prog(
        "var arr: Array<int> := [1, 2, 1]; "
        "var i := 0; "
        "while i < 200 do "
        "  try "
        "    var d := arr.ToDictionary(lambda(x: int) => x, lambda(x: int) => x * 2); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "return 0; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #5421: two-selector overload duplicate-key throw path (string keys).
BEZEL_TEST(todictionary_duplicate_key_leak_5421,
           two_selector_string_key_throw_path_no_closure_leak) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(prog(
        "var arr: Array<string> := [\"x\", \"y\", \"x\"]; "
        "var i := 0; "
        "while i < 200 do "
        "  try "
        "    var d := arr.ToDictionary(lambda(s: string) => s, lambda(s: string) => s.Length); "
        "  catch "
        "    var e: ArgumentException => do end "
        "  end; "
        "  i := i + 1; "
        "end while; "
        "return 0; "));
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Codegen — unimplemented paths must be hard compile errors (#628 part 1).
//
// Issue #628 (part 1): when codegen emits a "not yet implemented" diagnostic
// (Severity::Error), compilation must abort — no binary is produced and the
// JIT harness must see `ran == false`.  The historical bug was that
// `emitModule` returned the partially-built module even when errors had been
// recorded, so the driver (AOT path) proceeded to emit an object file and link
// a binary whose loop body had been silently dropped.
//
// This file covers the two codegen-stage "not yet implemented" sites that are
// reachable via the JIT test harness (the `for-statement` fallthrough and the
// `await for` fallthrough).  The `typeof` / `GetType` sites are already tested
// in tests/sema/typeof_gettype_487_test.cpp.
//
// Spec references:
//   §4.9 v0.135 — OwnedDictionary.Drain() owned-consuming iteration
//   §30.7        — `await for` async iteration

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runJit;

namespace {

// Helper: returns true when a source string fails to compile AND the JIT never
// executes.  This is the minimal "must be a hard error" assertion.
bool compileFails(const std::string& src) {
    JitResult r = runJit(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

}  // namespace

// ===========================================================================
// §4.9 / #628: `for var entry in ownedDict.Drain()` — OwnedDictionary drain
// iteration is not yet implemented (part 2 of #628 tracks the feature).  The
// compile must FAIL with a hard error and produce NO output; the loop must
// NOT be silently dropped.
// ===========================================================================

// §4.9 / #628: OwnedDictionary.Drain() in a for loop — must be a hard
// compile error (no binary produced, JIT never runs).
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_for_is_hard_error) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    d.Set(\"a\", 1); "
        "    var count := 0; "
        "    for var entry in d.Drain() do "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"));
}

// §4.9 / #628: Verify that the diagnostic engine recorded at least one error
// (not just that `ran` is false due to a JIT/link failure).
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_for_records_error) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    for var entry in d.Drain() do "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// §4.9 / #628: A program whose ONLY unimplemented construct is the Drain loop
// must fail — not partially succeed (exit code 0 with the loop dropped).
// ===========================================================================

// §4.9 / #628: With the bug, this returned exitCode=0 and ran=true (the loop
// was silently dropped).  After the fix: ran=false (compile fails).
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_does_not_silently_succeed) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    d.Set(\"x\", 42); "
        "    for var entry in d.Drain() do "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Must NOT have run — the loop is unimplemented, so the whole compile
    // must fail rather than silently producing a binary that returns 0.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

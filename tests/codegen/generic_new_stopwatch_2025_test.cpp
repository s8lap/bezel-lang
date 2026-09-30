// Codegen — #2025 (stdlib §8.1, §14.2, v0.197): `Factory.Make<Stopwatch>()` —
// a generic `where T: new()` factory instantiated with `Stopwatch` — ICEs at
// IR verification ("Function return type does not match operand type of return
// inst! `ret ptr %1` / `i32`").
//
// SPEC: stdlib §8.1 declares `Stopwatch` a `public class` with a
// `public constructor();`, so per §14.2 (v0.197) it satisfies `where T: new()`,
// and §14.2 makes `new T()` for a class substitution produce an owned reference
// via the class's parameterless constructor. So a generic `Make<Stopwatch>()`
// is a valid program and must construct + dispose a real Stopwatch.
//
// ROOT CAUSE: `bz_stopwatch_alloc` (the lowerNew dispatch for Stopwatch,
// LlvmBackend_Exprs2.cpp:3157) returns a ptr — but `Stopwatch` was MISSING from
// `kHeapPointerReferenceBuiltins` (src/common/BuiltinRegistry.cpp), so the type
// lowered to the i32 default (LlvmBackend_Layout.cpp calls
// isHeapPointerReferenceBuiltin at both the astNode and imported paths). At the
// factory call site the function's return type was i32 while the constructed
// operand was ptr → IR verification failed (ICE). Exposed by #2017, which made
// the generic `new T()` path actually reach lowerNew's Stopwatch dispatch.
//
// FIX: add "Stopwatch" to kHeapPointerReferenceBuiltins so `owned Stopwatch` /
// `T = Stopwatch` lowers to ptr consistently with bz_stopwatch_alloc's return.
//
// A leak / double-free / use-after-free / SEGV flips r.ran to false -> -999,
// distinct from any legal exit value; runAotChecked forces generated-code ASan.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The `where T: new()` factory whose body constructs `new T()` and returns it
// as `owned T` — the exact shape of the issue's repro. Mirrors the
// #2017 lane's factory, instantiated with Stopwatch here.
const char* kFactory =
    "static class Factory "
    "  public static method Make<T>(): owned T where T: new() "
    "    var x := new T(); "
    "    return x; "
    "  end Make; "
    "end Factory; ";
}  // namespace

// #2025 (stdlib §8.1, §14.2): the canonical repro. `Make<Stopwatch>()` must
// construct a real, usable Stopwatch and dispose it cleanly at scope end.
// Before the fix: lowerNew dispatched to bz_stopwatch_alloc (ptr) while the
// factory's `owned T` return lowered to i32 → IR verification failed (ICE,
// exit -999). With the fix: Start/Stop/IsRunning work, exit 1.
BEZEL_TEST(codegen_generic_new_stopwatch_2025,
           make_stopwatch_constructs_usable_stopwatch_spec_8_1) {
    std::string src =
        std::string("module M; import System.Diagnostics; ") + kFactory +
        "class C public static method Main(): int "
        "  using var sw := Factory.Make<Stopwatch>(); "
        "  sw.Start(); "
        "  sw.Stop(); "
        "  if not sw.IsRunning then return 1; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// #2025 sibling regression: a plain `using var sw := new Stopwatch();` (no
// generic, T = Stopwatch spelled directly) must still construct, read
// ElapsedMilliseconds (i64), and dispose. This pins that the registry fix
// keeps the direct-construction path sound (Stopwatch's lowerNew dispatch and
// Layout rep now agree on ptr).
BEZEL_TEST(codegen_generic_new_stopwatch_2025,
           make_stopwatch_sibling_direct_new_still_works_spec_8_1) {
    std::string src =
        std::string("module M; import System.Diagnostics; ") +
        "class C public static method Main(): int "
        "  using var sw := new Stopwatch(); "
        "  if sw.ElapsedMilliseconds = 0L then return 1; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

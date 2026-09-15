// Codegen / memory-unsafety — issue #4426 (spec §27.5 Closures / §28.9
// Runtime-managed types / §27.3 Lambda expressions, v0.300):
// assigning a chained call to a local var `var w := Make()();` leaks the
// Widget returned by the chained call (the local's disposal-registration
// slot diverges from CG60's temp slot holding the Widget pointer).
//
// Distinguished from #4239 (closed via 8c36478f predicate-bypass mechanism
// in r18-c7): that issue's verbatim `return MakeClosure()();` double-free is
// a release-counting defect in the same CG60 arm; this #4426 leak is an
// orthogonal slot-routing defect. Same machinery, different symptom, both
// real and reproducible at HEAD pre-fix.
//
// MECHANICAL ROOT CAUSE. `lowerLocalDecl` (LlvmBackend_Setup.cpp) processes
// `var w := <expr>` by calling `lowerExpr(*init)` to evaluate the
// initializer. For a plain call like `var w := M()` returning an owned class
// instance, `lowerExpr` returns the instance pointer, `lowerLocalDecl` stores
// it in `w`'s slot, and registers `w` for scope-end disposal. For a chained
// call `var w := M()()`, `lowerExpr` for the outer CallExpr goes through
// the function-typed call path (LlvmBackend_Calls2.cpp CG60 arm), which calls
// `emitClosureCall` and then `emitClosureRelease` for the closure. However,
// the result of `emitClosureCall` (the Widget from the closure) is used
// DIRECTLY without being stored through `lowerLocalDecl`'s slot —
// `call cont4` shows the Widget stored to a temp, and `lowerLocalDecl`'s
// store goes to a DIFFERENT slot (w's slot). The disposal registration
// is on w's slot but the Widget lives in the temp.
//
// THE LEAK. The Widget is stored to a temp slot by CG60, and w's slot gets
// a SEPARATE (uninitialized or null) value. At scope end, disposal fires on
// w's slot (which is null/garbage), while the temp holding the real Widget
// is never disposed.
//
// THE FIX. The CG60 arm stores the Widget through the local's OWN slot
// (wired from lowerLocalDecl via pendingChainCallLocalSlot_) and registers
// that slot for scope-end disposal. A second store in lowerLocalDecl is
// harmless (same Widget pointer); no double-disposal occurs because the
// pending flag is consumed by CG60.
//
// MEASURED (BEZEL_DUMP_IR=1, HEAD vs fix):
//   pre-fix: main has NO disposal for w — Widget leaks (ASan: 8000 B at ×500)
//   post-fix: main has using.cls.dispose6 block — ASan clean
//
// TEST NOTE. `return Widget4239.n` evaluates n BEFORE scope-end disposal,
// so the return value is unreliable for distinguishing pre/post. The 500x
// row (FAIL-WITHOUT) is the reliable oracle: pre-fix leaks 8000 B and
// ASan exits 244; post-fix is ASan-clean (exit 0).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::JitResult;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Disposal-counted class: `_n` is the live count.
// v0.221 §22.12 rule 2 form (iii) rejects mutable static primitives,
// so AtomicInteger<int> (a class type) is used instead.
// The return value is unreliable (evaluated before scope-end disposal) —
// the 500x row is the authoritative FAIL-WITHOUT oracle (ASan detects
// 8000 B leak pre-fix; post-fix is ASan-clean).
const char* kWidget =
    "class Widget4239 "
    "  private static readonly _n: AtomicInteger<int> := new AtomicInteger<int>(0); "
    "  public constructor() _n.Value := _n.Value + 1; end constructor; "
    "  destructor _n.Value := _n.Value - 1; end destructor; "
    "end Widget4239; ";

// The repro from issue #4426: a chained call `C.Make()()` assigned to a local
// where `Make` returns a non-capturing closure that produces a fresh owned
// Widget. Pre-fix: the Widget is never disposed (leak under ASan).
// Post-fix: the Widget is properly disposed (ASan clean).
std::string chainedCallProgram() {
    return std::string("module M; ") + kWidget +
        "class C4239 "
        "  public static method Make(): () -> Widget4239 "
        "    return lambda(): Widget4239 => new Widget4239(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var w := C4239.Make()(); "
        "    return 0; "
        "  end Main; "
        "end C4239;";
}

// Row 1: single-shot. Pre-fix: Widget leaks (ASan detects 16 B, exit ≠ 0).
// Post-fix: ASan clean (exit 0). runAotChecked runs under ASan, so a leak
// causes a non-zero exit even if the program itself exits 0.
BEZEL_TEST(chained_call_local_var_leak_4426,
           chained_call_local_var_single_shot_leak_free_4426) {
    JitResult r = runAotChecked(chainedCallProgram());
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 2: 500x amplification — the FAIL-WITHOUT row, amplified.
// Pre-fix: 500 * 16 B = 8000 B leaked (ASan exit 244).
// Post-fix: clean (exit 0 under ASan).
BEZEL_TEST(chained_call_local_var_leak_4426,
           chained_call_local_var_500x_leak_free_4426) {
    std::string prog =
        std::string("module M; ") + kWidget +
        "class C4239 "
        "  public static method Make(): () -> Widget4239 "
        "    return lambda(): Widget4239 => new Widget4239(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var w := C4239.Make()(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C4239;";
    JitResult r = runAotChecked(prog);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

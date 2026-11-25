// Ownership — the drop-on-overwrite of a REASSIGNED owned LOCAL disposes the
// displaced value when the local was INITIALISED from a FUNCTION-VALUE
// INVOCATION (`var w := h(); w := new W();`) — issue #9139.
//
// Spec, v0.359:
//   sec 28.3 (announcement table, row 1): "`owned T` over a non-mint-announcing
//   type ... Instance, extension, function-value invocation: Owned." So an
//   invocation through a function value whose return slot carries `owned`
//   produces an OWNED value, and the local it initialises is an OWNED local.
//   sec 28.7: "Assignment to an owned field or owned local disposes the
//   displaced value ... The right-hand side is evaluated first; then, if the
//   previous value is live (non-null - and, for a binding, not moved-from), it
//   is disposed; then the new value is stored. The disposal is null-guarded and
//   dynamically dispatched, exactly the machinery scope-end disposal already
//   uses."
//   sec 27.1: a function type's return slot may carry `owned`.
//
// THE DEFECT. Codegen's class-local drop-on-overwrite
// (`disposeDisplacedOwnedClassLocal`) admits a local only when
// `unwindOwnedLocalClass` classifies it, and that classifier's call arm keys
// on a MethodSymbol callee. A FUNCTION-VALUE callee — a local, a field, or
// another call's result — carries no MethodSymbol, so the local was never
// classified as an owned class local and the §28.7 disposal was never emitted:
// the displaced value leaked. The OWNERSHIP pass already classifies the same
// initializer as Owned (§28.3's announcement table row 1, read through the
// callee's function type's `funcReturnOwned` flag) and already reports the
// overwrite as COVERED (`localDropOnOverwriteCovered`), so the ownership side
// promised a disposal that codegen never emitted.
//
// MEASURED on the base (r30-c1 @ e97a8efbc), `bezelc --sanitize=address`, a
// 200-iteration loop: every function-value spelling below leaked 3200 B in
// 200 allocations and ran the destructor 200 times instead of 400, while the
// byte-identical DIRECT-CALL twin (`var w := C.Get();`, a method returning
// `owned W`) was clean.
//
// Every runtime row runs 200x under generated-code ASan/LSan (runAotChecked)
// and pins the exact stdout; the destructor counter is what makes the missing
// disposal observable without a sanitizer, and a leak makes `r.ran` false. The
// `runAot` row is the same program on the plain AOT oracle (the issue's own
// repro, stdout + exit code).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// stdout (trailing newline stripped) of a clean, ASan/LSan-checked run, or a
// marker naming the failure (compile error, non-zero exit incl. a leak
// report).
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// The program: `W` counts its own destructions in the static `W.ds`.
// `C.Make()` mints a `() -> owned W` function value, `C.Get()` is the
// DIRECT-CALL twin (a method returning `owned W`), and `C.G` is a
// field-typed function value.
const char* kPrelude = R"BZL(
module M;
public class W
  public static ds: int := 0;
  public v: int := 3;
  destructor W.ds := W.ds + 1; end destructor;
end W;
public class C
  public G: () -> owned W := lambda(): W => new W();
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Get(): owned W
    return new W();
  end Get;
end C;
)BZL";

// The loop body runs 200 times; each iteration mints one `owned W` through the
// shape under test and then REASSIGNS the local, so the displaced value must
// be disposed (§28.7) exactly once.
std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "public class App\n"
           "  public static method Main(): int\n"
           "    var sum := 0;\n"
           "    for var i in 0..<200 do\n" +
           body +
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString() + \" ds=\" + "
           "W.ds.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end App;\n";
}

}  // namespace

// --- #9139: the function-value invocations -----------------------------------

// The issue's own repro: the local is initialised from a FUNCTION-VALUE
// INVOCATION and then reassigned. Two `W` destructions per iteration — the
// displaced value at the store (§28.7) and the final value at scope end.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            function_value_call_reassign_disposes_the_displaced_value_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var h := C.Make();\n"
                 "      var w := h();\n"
                 "      sum := sum + w.v;\n"
                 "      w := new W();\n"
                 "      sum := sum + w.v;\n")),
        "sum=1200 ds=400");
}

// The CHAIN spelling: the callee is another call's result (`C.Make()()`),
// whose type is the same `() -> owned W` function type.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            chain_call_reassign_disposes_the_displaced_value_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var w := C.Make()();\n"
                 "      sum := sum + w.v;\n"
                 "      w := new W();\n"
                 "      sum := sum + w.v;\n")),
        "sum=1200 ds=400");
}

// The FIELD spelling: the callee is a function-typed FIELD (`a.G()`), the
// same function value read out of storage.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            field_function_value_reassign_disposes_the_displaced_value_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var a := new C();\n"
                 "      var w := a.G();\n"
                 "      sum := sum + w.v;\n"
                 "      w := new W();\n"
                 "      sum := sum + w.v;\n")),
        "sum=1200 ds=400");
}

// The METHOD-REFERENCE spelling: the local holds a method reference
// (`C.Get`, whose formed type is `() -> owned W`) and the call is through it.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            method_reference_local_reassign_disposes_the_displaced_value_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var f := C.Get;\n"
                 "      var w := f();\n"
                 "      sum := sum + w.v;\n"
                 "      w := new W();\n"
                 "      sum := sum + w.v;\n")),
        "sum=1200 ds=400");
}

// The same program on the PLAIN AOT oracle: the issue's repro, with the
// destructor counter added so the missing disposal is visible in stdout
// alone. RED on the base ("ds=200"), GREEN with the fix ("ds=400").
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            function_value_call_reassign_disposes_the_displaced_value_runAot_200x) {
    auto r = runAot(prog("      var h := C.Make();\n"
                         "      var w := h();\n"
                         "      sum := sum + w.v;\n"
                         "      w := new W();\n"
                         "      sum := sum + w.v;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    BEZEL_EXPECT_EQ(s, "sum=1200 ds=400");
}

// --- the controls -------------------------------------------------------------

// The DIRECT-CALL twin: `C.Get()` is a METHOD call returning `owned W`, so
// the local was already an owned class local and the displaced value was
// already disposed. It must stay that way — this row is green on the base.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            control_direct_method_call_reassign_disposes_the_displaced_value_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var w := C.Get();\n"
                 "      sum := sum + w.v;\n"
                 "      w := new W();\n"
                 "      sum := sum + w.v;\n")),
        "sum=1200 ds=400");
}

// The SINGLE-BINDING control: the same function-value invocation with NO
// reassignment disposes its value exactly once, at scope end — the new
// classification must not dispose it twice.
BEZEL_TEST(ownership_function_value_call_reassign_drop_on_overwrite_9139,
            control_single_binding_disposes_once_200x) {
    BEZEL_EXPECT_EQ(
        out(prog("      var h := C.Make();\n"
                 "      var w := h();\n"
                 "      sum := sum + w.v;\n")),
        "sum=600 ds=200");
}

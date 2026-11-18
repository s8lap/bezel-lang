// Codegen — issue #8854: a function value passed to a CLASS-LEVEL generic
// method's parameter (`Keep<T>.Put(a: T)`) was never released: 64 B per call,
// capture-free and bound to a local.
//
// THE RULE. §28.3 *Set-erasing positions* (v0.359) names this exact call —
// "a field (instance or static; not a `scoped` field, which is declared only
// at a function type, §28.14), parameter, return slot, `ref` or `out`
// location, record component, choice payload or inline-array slot whose
// declared, pre-substitution type (the element type, for an inline array) is
// a bare type parameter, at an instantiation substituting a function type or
// `Object`, nullable or not, for it — `k.Put(f)` at `k: Keep<Object>` over
// `Put(a: T)`" — and rules: "At every set-erasing position a function value
// with a non-empty source set is rejected, and a capture-free one is
// admitted".
//
// §28.3 *Announcement-erasing positions* (v0.359): such a position "announces
// no source set", so the callee holds no reference past the call and the
// CALLER keeps the obligation. §28.14 (v0.359): "A function-typed parameter
// declared *without* `scoped` is **call-bounded** (the callback is scoped to
// the call)" — "the callee receives the callback for the duration of the call
// only". §28.9 (v0.359): a function value's lifetime "is managed by the
// runtime without user involvement" — the heap-allocated closure object — and
// §27.5 (v0.359) is the closure object itself. So the caller's local holds
// the only reference and must release it at its scope end.
//
// ROOT CAUSE. The caller's escape walk
// (`Emitter::closureArgBindsMethodOwnTypeParamBorrow`,
// src/codegen/LlvmBackend_Setup.cpp) is the "the argument is a BORROW, the
// caller keeps its +1" relief. Its arm (a) admits a formal declared at the
// callee METHOD's OWN generic parameter (`G<T>(a: T)`), but there was no arm
// for a formal declared at the ENCLOSING TYPE's generic parameter
// (`Keep<T>.Put(a: T)`); arms (b)/(c) cannot fire either, because the
// formal's DECLARED type is the bare `Named` head `T` — never a resolved
// function type, never `Primitive Object`. The argument was therefore
// classified as an ESCAPE, the caller's scope-end `bz_closure_release` was
// never armed, and the 64-byte closure cell leaked on every call.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1; the
// issue's own 200-iteration loop):
//
//   row                                              base        with the fix
//   issue_repro_verbatim_spec_28_9                   12800 B/200 clean, "ran"
//   function_type_substitution_spec_28_3              12800 B/200 clean, "ran"
//   using_var_receiver_spec_28_9                     12800 B/200 clean, "ran"
//   method_own_generic_twin_clean_spec_28_14         clean       clean
//   storing_callee_keeps_escape_no_uaf_spec_28_3     clean       clean
//
// The output is identical on both arms of every row.
//
// The two controls are the suite's at-most-two boundary controls (lane
// contract §7): each is green on the base and each goes RED under a
// plausible wrong fix — see the two paragraphs above the rows.
//
// The issue's own base-green controls (the same loop WITHOUT the `k.Put(fq)`
// call, and the non-generic `Take(o: Object)` with the same bound local) are
// not rows here: a green-on-both-arms gtest row pins nothing. They are in
// the lane's corpus-candidates directory with the spec sentence that decides
// them.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no sanitizer
// report fired (AddressSanitizer and LeakSanitizer both on); a marker
// otherwise. A leak on the base arm makes the sanitizer fire, so the marker
// is what the base arm prints and the expected text is what the fixed arm
// prints — the row is RED on the base for that reason alone.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// The issue's own repro, with the class's type argument and the receiver
// spelling as parameters. `Keep<T>.Put(a: T)` is the class-generic formal;
// `G<T>(a: T)` is the METHOD-level generic twin the issue compares against.
std::string classGenericRepro(const std::string& typeArg,
                              const std::string& receiver) {
    return R"BZL(
module M;
import System;
public class Keep<T>
  public constructor() end constructor;
  public method Put(a: T) end Put;
end Keep;
public class Program
  public static method Main(): int
    for var i in 0 ..< 200 do
)BZL" + receiver + R"BZL(
      var fq := lambda() => 3;
      k.Put(fq);
    end for;
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)BZL";
}

}  // namespace

// ===========================================================================
// The issue's repro, verbatim: `Keep<Object>.Put(fq)` with `fq` a
// capture-free closure local bound before the call. §28.3 *Set-erasing
// positions* (v0.359) admits the capture-free function value at that
// position and announces no source set there, so the caller's local keeps
// the only reference and must release the closure at its scope end.
//
// runAotChecked (generated-code AddressSanitizer + LeakSanitizer): on the
// base the 200 calls leak 12800 B in 200 allocations and the sanitizer
// fires; with the fix the program prints "ran" and exits 0.
// ===========================================================================
BEZEL_TEST(class_generic_fn_value_param_release_8854,
           issue_repro_verbatim_spec_28_9) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(classGenericRepro("Object", "var k := new Keep<Object>();\n")),
        "ran\n");
}

// ===========================================================================
// The same repro at the OTHER substitution §28.3's bullet names: the class's
// type argument is a FUNCTION TYPE (`Keep<() -> int>`), so the formal is
// function-typed at this instantiation and §28.14's call-bounded default
// applies directly ("A function-typed parameter declared *without* `scoped`
// is **call-bounded**").
//
// RED on the base (12800 B / 200), clean with the fix.
// ===========================================================================
BEZEL_TEST(class_generic_fn_value_param_release_8854,
           function_type_substitution_spec_28_3) {
    BEZEL_EXPECT_EQ(runCheckedOut(classGenericRepro(
                        "() -> int", "var k := new Keep<() -> int>();\n")),
                    "ran\n");
}

// ===========================================================================
// The issue's third measured spelling: `using var k` instead of `var k` for
// the receiver. §28.9 (v0.359): a `using var` local of function type "is
// nonetheless permitted (disposal at scope end is a no-op)"; here the
// receiver is a class instance, and the difference is only in the receiver's
// own disposal point — the closure leak is the same and must be gone.
//
// RED on the base (12800 B / 200), clean with the fix.
// ===========================================================================
BEZEL_TEST(class_generic_fn_value_param_release_8854,
           using_var_receiver_spec_28_9) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(classGenericRepro(
            "Object", "using var k := new Keep<Object>();\n")),
        "ran\n");
}

// ===========================================================================
// CONTROL 1 of the suite's two — the METHOD-level generic twin the issue
// compares against: `G<T>(a: T)` called at `G<() -> int>(fq)` is CLEAN on the
// base, because arm (a) of the escape walk already admits a method-own
// type-parameter formal. It is green on BOTH arms and is here to pin that the
// fix does not disturb it.
//
// It goes RED under a plausible wrong fix: one that also releases the
// closure at the CALL's end (the call-site temporary release) on top of the
// scope-end release — a double free, which the generated-code ASan reports.
// ===========================================================================
BEZEL_TEST(class_generic_fn_value_param_release_8854,
           method_own_generic_twin_clean_spec_28_14) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(R"BZL(
module M;
import System;
public class Program
  public static method G<T>(a: T): int
    return 0;
  end G;
  public static method Main(): int
    for var i in 0 ..< 200 do
      var fq := lambda() => 3;
      G<() -> int>(fq);
    end for;
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)BZL"),
        "ran\n");
}

// ===========================================================================
// CONTROL 2 of the suite's two — the callee that STORES the formal. §28.3's
// sink rule admits a CAPTURE-FREE function value at a set-erasing position
// ("A capture-free function value is admitted at each of those positions"),
// so a class-generic callee may keep the argument in a field of its own.
// The field is a BORROW of the caller's cell (the field store does not take
// a reference of its own), so the caller's release must stay WITHHELD: the
// caller's local is the only owner, and releasing it would free the cell the
// callee's field still points at.
//
// The row therefore runs under runAotCheckedLenient (LeakSanitizer OFF — the
// leak is the SAFE direction here and is expected) and pins the NO-UAF half:
// the field is invoked in the SECOND iteration, after the first iteration's
// local has gone out of scope, so a wrong fix that arms the caller's release
// without the callee-body walk frees the cell the field still points at and
// the second invocation is a heap-use-after-free.
//
// Green on both arms (the release stays withheld either way).
// ===========================================================================
BEZEL_TEST(class_generic_fn_value_param_release_8854,
           storing_callee_keeps_escape_no_uaf_spec_28_3) {
    auto r = runAotCheckedLenient(
        "module M; "
        "import System; "
        "public class Keep<T> "
        "  public F: T?; "
        "  public constructor() end constructor; "
        "  public method Put(a: T) F := a; end Put; "
        "end Keep; "
        "public class Program "
        "  public static method Main(): int "
        "    using var k := new Keep<() -> int>(); "
        "    var total := 0; "
        "    for var i in 0 ..< 2 do "
        "      var fq := lambda() => 3; "
        "      k.Put(fq); "
        "      total := total + k.F!(); "
        "    end for; "
        "    Console.WriteLine($\"{total}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

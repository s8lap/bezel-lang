// Codegen — issue #8900: a lambda LITERAL passed straight to a CLASS-level
// generic method's bare-type-parameter formal (`Keep<T>.Put(a: T)`) was never
// released: 64 B per call.
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
// §27.5 (v0.359) is the closure object itself. So the caller's temporary
// holds the only reference and must release it when the call returns.
//
// ROOT CAUSE. The call-site temporary release
// (`Emitter::prepareCallClosureArgRelease`'s call-bounded arm,
// src/codegen/LlvmBackend_Calls2.cpp) admits a bare-`Named` formal only when
// the Named head is one of the callee METHOD's OWN `genericParams` — the
// #5116 narrowing, kept for a documented reason (the #1921/#2248 UAF
// cluster). A formal declared at the ENCLOSING TYPE's generic parameter
// (`Keep<T>.Put(a: T)`) was therefore skipped, the arm never fired, and the
// literal's +1 was dropped by nobody.
//
// THIS IS NOT #8854's SITE. #8854 fixed the BOUND-LOCAL spelling (the closure
// bound to a local first), whose missing release is the caller's SCOPE-END
// release (`Emitter::closureArgBindsMethodOwnTypeParamBorrow`,
// src/codegen/LlvmBackend_Setup.cpp). A LITERAL has no local, so it needs the
// CALL-SITE release above: a different function, a different mechanism, a
// different proof obligation. Nor is it #8843's site (a literal at a declared
// `Object` formal): that predicate declines generic callees on purpose.
//
// MEASURED (`bezelc --sanitize=address`, detect_leaks=1; the issue's own
// 200-iteration loop):
//
//   row                                     base            with the fix
//   issue_repro_verbatim_spec_28_9          12800 B/200    clean, "ran"
//   function_type_substitution_spec_28_3     12800 B/200    clean, "ran"
//   using_var_receiver_spec_28_9            12800 B/200    clean, "ran"
//   bound_local_sibling_no_double_free_28_14 clean          clean
//   storing_callee_no_uaf_spec_28_3          clean          clean
//
// The output is identical on both arms of every row.
//
// The last two rows are the suite's at-most-two boundary controls (lane
// contract §7): each is green on the base and each goes RED under a plausible
// wrong fix — see the paragraphs above the rows.
//
// The issue's own base-green controls (the same loop WITHOUT the `k.Put(...)`
// call, and the non-generic `Take(o: Object)` with a literal) are not rows
// here: a green-on-both-arms gtest row pins nothing. They are in the lane's
// corpus-candidates directory with the spec sentence that decides them.

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

// The issue's repro, with the class's type argument and the receiver
// spelling as parameters. `Keep<T>.Put(a: T)` is the class-generic formal.
std::string classGenericLiteralRepro(const std::string& typeArg,
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
      k.Put(lambda() => 3);
    end for;
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)BZL";
}

}  // namespace

// ===========================================================================
// The issue's repro, verbatim: `Keep<Object>.Put(lambda() => 3)` — the
// lambda LITERAL passed straight to the class-generic formal. §28.3
// *Set-erasing positions* (v0.359) admits the capture-free function value at
// that position and announces no source set there, so the callee holds no
// reference past the call and the caller's temporary keeps the only
// reference and must release the closure when the call returns.
//
// runAotChecked (generated-code AddressSanitizer + LeakSanitizer): on the
// base the 200 calls leak 12800 B in 200 allocations and the sanitizer
// fires; with the fix the program prints "ran" and exits 0.
// ===========================================================================
BEZEL_TEST(class_generic_literal_fn_value_param_release_8900,
           issue_repro_verbatim_spec_28_9) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(classGenericLiteralRepro(
            "Object", "var k := new Keep<Object>();\n")),
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
BEZEL_TEST(class_generic_literal_fn_value_param_release_8900,
           function_type_substitution_spec_28_3) {
    BEZEL_EXPECT_EQ(runCheckedOut(classGenericLiteralRepro(
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
BEZEL_TEST(class_generic_literal_fn_value_param_release_8900,
           using_var_receiver_spec_28_9) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(classGenericLiteralRepro(
            "Object", "using var k := new Keep<Object>();\n")),
        "ran\n");
}

// ===========================================================================
// CONTROL 1 of the suite's two — the BOUND-LOCAL sibling, #8854's shape: the
// closure is bound to a local first and the local is passed to the SAME
// class-generic formal. #8854's fix discharges it at the LOCAL's scope end,
// so it is clean on the base and must stay clean.
//
// It goes RED under a plausible wrong fix: one that arms the call-site
// release for ANY argument at a class-generic bare-type-parameter formal
// instead of only for a FRESH +1 producer. The bound local would then be
// released twice — once at the call's end and once at the scope end — a
// double free, which the generated-code ASan reports.
//
// Green on both arms.
// ===========================================================================
BEZEL_TEST(class_generic_literal_fn_value_param_release_8900,
           bound_local_sibling_no_double_free_spec_28_14) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(R"BZL(
module M;
import System;
public class Keep<T>
  public constructor() end constructor;
  public method Put(a: T) end Put;
end Keep;
public class Program
  public static method Main(): int
    for var i in 0 ..< 200 do
      var k := new Keep<Object>();
      var fq := lambda() => 3;
      k.Put(fq);
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
// The field is a BORROW of the caller's cell (a `T?` field store takes no
// reference of its own — codegen does not recognise a type-parameter field
// as a closure storage member), so the call-site release must stay
// WITHHELD: releasing it would free the cell the callee's field still points
// at.
//
// The row therefore runs under runAotCheckedLenient (LeakSanitizer OFF — the
// leak is the SAFE direction here and is expected) and pins the NO-UAF half:
// the field is invoked in the SECOND iteration, after the first iteration's
// literal has been released, so a wrong fix that arms the call-site release
// without the callee-body walk frees the cell the field still points at and
// the second invocation is a heap-use-after-free.
//
// This is the same shape as the in-suite control that already pins the safe
// direction for the class-own formal (`codegen_1073_closure_arg.
// bare_type_parameter_sink_not_released_spec_28_14`, whose callee stores a
// BOUND LOCAL); this row is the LITERAL spelling of it.
//
// Green on both arms (the release stays withheld either way).
// ===========================================================================
BEZEL_TEST(class_generic_literal_fn_value_param_release_8900,
           storing_callee_no_uaf_spec_28_3) {
    auto r = runAotCheckedLenient(
        "module M; "
        "import System; "
        "public class Box<T> "
        "  public F: T?; "
        "  public constructor() end constructor; "
        "  public method Keep(x: T) "
        "    F := x; "
        "  end Keep; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new Box<() -> int>(); "
        "    var total := 0; "
        "    for var i in 0 ..< 2 do "
        "      b.Keep(lambda() => 42); "
        "      total := total + b.F!(); "
        "    end for; "
        "    Console.WriteLine($\"{total}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

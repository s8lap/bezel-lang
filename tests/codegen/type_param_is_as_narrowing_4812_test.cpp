// Regression tests for issue #4812 — §12.7 `is`/`as` on a TYPE-PARAMETER-typed
// simple variable was wrong code at base, and #4768's arm (fix #4768,
// tests/sema/type_param_to_primitive_4768_test.cpp) then turned part of it
// into an over-rejection.
//
// Spec (v0.307): §12.7 — "When the test succeeds and the tested expression is
// a simple variable, the variable is flow-narrowed to `T` within the
// controlled block." Nothing restricts this to non-generic carriers, so a
// `v: T` tested with `is int` is `int` inside the block — the same rule the
// compiler already applies when the declared location is `Object`.
//
// MEASURED on origin/main (e9eac9cc), before this fix:
//   * Repro A (`if v is int then var x: int := v; …` at `Holder<int>`):
//     REJECTED — "`T` is a type parameter, and a generic body converts a
//     type-parameter value only to `Object`…" (the #4768 arm firing on the
//     UNNARROWED declared type, because sema never narrowed `v` at all).
//   * Repro B (`if v is string then var s: string := v; return s.Length; …`
//     at `Holder<string>`): the same #4768 over-rejection.
//   * Repro C (`var n: int? := v as int?; return n ?? -1;` at `Holder<int>`,
//     `Peek(7)`): compiled, printed `-1` (wrong code — `n` never held `7`).
//
// The fix has two parts:
//   1. sema (ExprTypecheck.cpp, narrowFromTypeTest): a bare type-parameter
//      DECLARED location is now an admitted §12.7 narrowing carrier for a
//      PRIMITIVE test type (mirroring the existing Object/interface rule),
//      so the narrowed read publishes the tested type and #4768's arm never
//      sees the unnarrowed `T` for it.
//   2. codegen: the SPECIALIZED (monomorphized) body already resolves a bare
//      type-parameter operand through `genericSubstSemaTy_`
//      (LlvmBackend_Layout.cpp, widen #4925's `opTyResolved`) reused here for
//      the non-pointer concrete-value-operand branch — this is what makes the
//      runtime `is`/`as` test and the narrowed read correct once
//      monomorphized. The UNSPECIALIZED generic template stub (issue #490 —
//      "never executed") has no substitution to consult, so a narrowed T-read
//      into a differently-shaped declared local (e.g. `var s: string := v;`
//      for an erased-to-`i32` `v: T`) is degraded to the existing #2434
//      zero-init answer (LlvmBackend_Setup.cpp, lowerLocalDecl) instead of
//      emitting the mismatched-type store that crashed the IR verifier on a
//      block that never runs.
//
// ORACLE: every accept row RUNS and asserts EXACT stdout (the corrected
// value, not merely "did not crash"). The reject control re-asserts #4768's
// own wording so an UNNARROWED type-parameter read still fails the same way.
//
// Cite: spec §12.7, v0.307 (narrowing); §5.1/§3.6 rule 2/§14.2, v0.307 (the
// #4768 arm, re-affirmed for the un-narrowed case).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

std::string runStdout(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did not run>");
    return r.stdoutText;
}

}  // namespace

// Repro A: `is int` narrowing on a type-parameter-typed variable at
// `Holder<int>`. Pre-fix: rejected by #4768's arm.
BEZEL_TEST(type_param_is_as_narrowing_4812, is_int_narrowing_at_int_instantiation_spec_12_7) {
    BEZEL_EXPECT_EQ(runStdout(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    if v is int then
      var x: int := v;
      return x;
    end;
    return 0;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<int>();
    Console.WriteLine(h.Peek(7));
  end Main;
end Program;
)BZ"), std::string("7\n"));
}

// Repro B: `is string` narrowing at `Holder<string>`, reading a member
// (`.Length`) of the narrowed local. Pre-fix: rejected by #4768's arm (and,
// before #4768 existed at all, an IR-verifier abort).
BEZEL_TEST(type_param_is_as_narrowing_4812, is_string_narrowing_at_string_instantiation_spec_12_7) {
    BEZEL_EXPECT_EQ(runStdout(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    if v is string then
      var s: string := v;
      return s.Length;
    end;
    return 0;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<string>();
    Console.WriteLine(h.Peek("abcd"));
  end Main;
end Program;
)BZ"), std::string("4\n"));
}

// Repro C: `as int?` on a type-parameter-typed variable at `Holder<int>`.
// Pre-fix: compiled, but printed `-1` (wrong code — the coalesced default,
// because `n` never actually held `7`).
BEZEL_TEST(type_param_is_as_narrowing_4812, as_nullable_int_at_int_instantiation_spec_12_7) {
    BEZEL_EXPECT_EQ(runStdout(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    var n: int? := v as int?;
    return n ?? -1;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<int>();
    Console.WriteLine(h.Peek(7));
  end Main;
end Program;
)BZ"), std::string("7\n"));
}

// Negative control: at `Holder<string>`, `v is int` is false — the narrowing
// carrier admits the test, but the monomorphized runtime answer is `false`,
// so the guarded block never runs and the method returns its `0` fallthrough.
// (This exercises the SAME statically-dead-branch codegen path as the
// unspecialized template stub, but at a concrete, executed instantiation —
// pins that the #4812 codegen degrade for a mismatched narrowed read does not
// merely avoid crashing but produces the SPEC-CORRECT value.)
BEZEL_TEST(type_param_is_as_narrowing_4812, is_int_false_at_string_instantiation_control_spec_12_7) {
    BEZEL_EXPECT_EQ(runStdout(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    if v is int then
      var x: int := v;
      return x;
    end;
    return 0;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<string>();
    Console.WriteLine(h.Peek("abcd"));
  end Main;
end Program;
)BZ"), std::string("0\n"));
}

// Negative control: the same narrowing on a declared `Object` carrier — the
// pre-existing rule #4812 mirrors — still runs unchanged.
BEZEL_TEST(type_param_is_as_narrowing_4812, object_carrier_still_narrows_control_spec_12_7) {
    BEZEL_EXPECT_EQ(runStdout(R"BZ(
module Conf;
public class Program
  public static method Peek(v: Object): int
    if v is int then
      var x: int := v;
      return x;
    end;
    return 0;
  end Peek;
  public static method Main()
    var o: Object := 7;
    Console.WriteLine(Peek(o));
  end Main;
end Program;
)BZ"), std::string("7\n"));
}

// Reject control: #4768's own arm must still fire on an UNNARROWED
// type-parameter read — #4812 narrows only INSIDE a proven `is T` block, not
// the bare declared type everywhere. Re-run of
// type_param_to_primitive_4768's `reference_arg_rejected_not_ir_abort_spec_5_1`
// repro, to pin that this fix did not widen the narrowing beyond its `is`/`as`
// controlled block.
BEZEL_TEST(type_param_is_as_narrowing_4812, unnarrowed_read_still_rejected_by_4768_spec_5_1) {
    auto r = runAot(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    var x: int := v;
    return x;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<int>();
    Console.WriteLine(h.Peek(7));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    bool foundRuleWording = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(
                "`T` is a type parameter, and a generic body converts a "
                "type-parameter value only to `Object`") != std::string::npos) {
            foundRuleWording = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(foundRuleWording);
}

// Regression tests for issue #4768 — a bare TYPE PARAMETER at a concrete-
// primitive target was silently accepted by `checkAssignable`'s catch-all.
//
// MEASURED (issue body at e443f8dc; re-measured at r20-c25 with `bezelc -c`):
//
//   * `class Box<Array> … var t: int := v;` (v: the parameter `Array`),
//     instantiated `Box<int>` — compiled clean and printed 42 (payload
//     reinterpreted; rc=0 at r20-c25).
//   * `class Holder<T> … var x: int := v;` instantiated `Holder<string>` —
//     codegen aborted the IR verifier: "Function return type does not match
//     operand type of return inst!" (rc=141 at r20-c25).
//
// Spec (v0.307): §5.1's only conversion out of a type parameter is "to an
// interface or class its constraints guarantee conformance to … inside the
// generic body a `T`-typed value converts to the bound" (plus `Object`), and
// §3.6 rule 2 says "a generic body type-checks under the weakest assumption
// its constraints establish". No constraint names a primitive, so the
// conversion does not exist under any constraint set, and the program is
// rejected at the DECLARATION.
//
// ORACLE. Reject rows assert rejection AND the arm's own wording (unique in
// src/). The Holder<string> row pins the specific diagnostic — not merely
// "did not run" — because the pre-fix failure was itself a compile failure
// (the IR-verifier abort), so a bare `!ran` would pass on both sides.
// Accept controls RUN and assert exact stdout.
//
// Cite: spec §5.1, §3.6 rule 2, §14.2, v0.307.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

constexpr const char* kRuleWording =
    "is a type parameter, and a generic body converts a type-parameter value "
    "only to `Object` and to the bounds its constraints guarantee";

bool rejectedByRule(const std::string& program, const char* paramName) {
    auto r = runAot(program);
    if (r.ran) return false;
    const std::string head = std::string("`") + paramName + "` ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(head + kRuleWording) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// The issue's first repro: a parameter that SHADOWS a built-in name, value
// type argument.
BEZEL_TEST(type_param_to_primitive_4768, shadowing_param_value_arg_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Box<Array>
  public method Pick(v: Array): int
    var t: int := v;
    return t;
  end Pick;
end Box;
public class Program
  public static method Main()
    using var b := new Box<int>();
    Console.WriteLine(b.Pick(42));
  end Main;
end Program;
)BZ", "Array"));
}

// The issue's second repro: reference type argument (pre-fix: IR-verifier
// abort).
BEZEL_TEST(type_param_to_primitive_4768, reference_arg_rejected_not_ir_abort_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Holder<T>
  public method Peek(v: T): int
    var x: int := v;
    return x;
  end Peek;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<string>();
    Console.WriteLine(h.Peek("abcd"));
  end Main;
end Program;
)BZ", "T"));
}

// Other targets and positions: `string` (§4.6), return value, argument, and a
// method-level type parameter; a constraint does not change the answer
// (§5.1 converts to the BOUND, which is an interface, never a primitive).
BEZEL_TEST(type_param_to_primitive_4768, other_targets_and_positions_rejected_spec_3_6) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Holder<T>
  public method Show(v: T): string
    var s: string := v;
    return s;
  end Show;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<int>();
    Console.WriteLine(h.Show(1));
  end Main;
end Program;
)BZ", "T"));
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Unwrap<U>(v: U): int
    return v;
  end Unwrap;
  public static method Main()
    Console.WriteLine(Unwrap<int>(3));
  end Main;
end Program;
)BZ", "U"));
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Take(n: long): long
    return n;
  end Take;
  public static method Pass<U>(v: U): long where U: IComparable<U>
    return Take(v);
  end Pass;
  public static method Main()
    Console.WriteLine(Pass<int>(3));
  end Main;
end Program;
)BZ", "U"));
}

// ═════════ Accept controls ═════════════════════════════════════════════════

// T -> Object (boxing, §5.1), T -> T, and a concrete-typed generic call
// result into a primitive inside a generic body (a callee's parameter, bound
// or inferred — not a type-parameter value of THIS body) still run.
BEZEL_TEST(type_param_to_primitive_4768, sanctioned_spellings_run_control) {
    auto r = runAot(R"BZ(
module Conf;
public class Util
  public static method Id<V>(x: V): V
    return x;
  end Id;
end Util;
public class Holder<T>
  public method Go(v: T): int
    var o: Object := v;
    var same: T := v;
    var n: int := Util.Id<int>(5);
    var m: int := Util.Id(6);
    return n + m;
  end Go;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder<string>();
    Console.WriteLine(h.Go("x"));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

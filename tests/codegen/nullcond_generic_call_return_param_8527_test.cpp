// fix #8527 (§14.5 phase 2 / §7.5, v0.357). A `?.` call of a generic method
// whose type parameter is bound ONLY by the lambda's return type, and whose
// declared return type is the BARE type parameter, returned garbage.
//
// §14.5 phase 2 ("Function-typed arguments, left to right"): "the lambda's
// inferred return type then unifies against the function type's return
// position, binding type parameters still free". So `R` in
// `Ap<T, R>(x: T, f: (T) -> R): R` is bound to the lambda's return type —
// `int` for `lambda(v: int) => v * 2`.
//
// Codegen had no arm for a function-typed parameter (sema's own
// `unifyArgSubst` has one), so `R` stayed unbound at the call site and was
// recovered from the call's RESULT type instead. At a `?.` call (§7.5: "The
// result type is always nullable") the result type carries the operator's
// extra nullable, so `R` was bound to `int?` where the lambda returns `int`:
// the monomorphized callee returned a nullable carrier (`{i8, i32}`) while
// the lambda returned a plain `int`, and the call read the two as one value —
// the garbage the issue prints.
//
// The rows below: the issue's own repro (RED on the base), the `.`-form
// control (GREEN on the base — the same program without the `?.`), the
// nullable-lambda shape whose `R` IS legitimately nullable (GREEN on the
// base — it must stay green), the null receiver (§7.5's short-circuit), and
// a string `R` (GREEN on the base).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim: `h?.Id(21)`, `h?.Ap<int, int>(...)` and
// `h?.Ap(5, lambda(v: int) => v * 2)`.
const char kIssueRepro[] = R"(
module P;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
  public method Id<T>(x: T): T
    return x;
  end Id;
end Helper;
public class Program
  public static method Main()
    var h: Helper? := new Helper();
    Console.WriteLine(h?.Id(21) ?? -1);
    Console.WriteLine(h?.Ap<int, int>(5, lambda(v: int) => v * 2) ?? -1);
    Console.WriteLine(h?.Ap(5, lambda(v: int) => v * 2) ?? -1);
  end Main;
end Program;
)";

// The same call without the `?.` — the control the issue's fix must not
// change (it is right on the base).
const char kDotForm[] = R"(
module P;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var h: Helper := new Helper();
    Console.WriteLine(h.Ap(5, lambda(v: int) => v * 2));
  end Main;
end Program;
)";

// A lambda whose return type is NULLABLE: `R` is legitimately `int?` here
// (§14.5 binds nullability as part of the binding), so the `?.` call's
// nullable result type is NOT an extra lift — the call must keep working.
const char kNullableLambdaReturn[] = R"(
module P;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var h: Helper? := new Helper();
    Console.WriteLine(h?.Ap(5, lambda(v: int) => do var t: int? := null; if v > 3 then t := v; end; return t; end) ?? -1);
  end Main;
end Program;
)";

// §7.5: a null receiver skips the call entirely — the result is null.
const char kNullReceiver[] = R"(
module P;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var h: Helper? := null;
    Console.WriteLine(h?.Ap(5, lambda(v: int) => v * 2) ?? -1);
  end Main;
end Program;
)";

// A string `R` — the neighbouring shape the issue measured as right.
const char kStringReturn[] = R"(
module P;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var h: Helper? := new Helper();
    Console.WriteLine(h?.Ap("ab", lambda(v: string) => v + "!") ?? "?");
  end Main;
end Program;
)";

// The issue's second shape: an UN-ANNOTATED lambda at a `?.` call. The
// callee's type parameters must be bound before the arguments are lowered,
// or the lambda's parameter keeps the int placeholder and `v.Age` reads off
// an int (the issue measured `0` where `80` is right).
const char kUnannotatedLambdaClassParam[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var b: Box := new Box("cho", 40);
    var h: Helper? := new Helper();
    Console.WriteLine(h?.Ap(b, lambda(v) => v.Age * 2) ?? -1);
  end Main;
end Program;
)";

// The same shape with an EXPLICIT type-argument list (`<Box, int>`) — the
// spelling the issue measured as printing `0`.
const char kExplicitTypeArgumentsClassParam[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public class Helper
  public method Ap<T, R>(x: T, f: (T) -> R): R
    return f(x);
  end Ap;
end Helper;
public class Program
  public static method Main()
    var b: Box := new Box("cho", 40);
    var h: Helper? := new Helper();
    Console.WriteLine(h?.Ap<Box, int>(b, lambda(v) => v.Age * 2) ?? -1);
  end Main;
end Program;
)";

}  // namespace

// The issue's repro. On the base the third line printed garbage (the value
// the mis-typed call read out of the mismatched carrier).
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_nullcond_call_binds_return_param) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "21\n10\n10\n");
}

// The `.`-form control: the same program without the `?.`. Right before the
// fix and right after it.
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_dot_form_control) {
    auto r = runAot(kDotForm);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}

// A nullable `R` is a legitimate binding (§14.5: "Nullability is therefore
// part of a binding") — the `?.` lift must not be mistaken for it.
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_nullcond_call_nullable_return_param) {
    auto r = runAot(kNullableLambdaReturn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// §7.5's short-circuit: a null receiver skips the call.
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_7_5_nullcond_call_on_null_receiver) {
    auto r = runAot(kNullReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// A string `R` at a `?.` call.
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_nullcond_call_string_return_param) {
    auto r = runAot(kStringReturn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ab!\n");
}

// An un-annotated lambda at a `?.` call: the callee's type parameters must
// be bound before the arguments are lowered, or the lambda's parameter
// keeps the int placeholder. On the base this printed `0` (the issue's
// second shape).
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_nullcond_call_unannotated_lambda) {
    auto r = runAot(kUnannotatedLambdaClassParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "80\n");
}

// The same shape with an explicit type-argument list — the spelling the
// issue measured as printing `0`.
BEZEL_TEST(codegen_nullcond_generic_call_return_param_8527,
           spec_14_5_phase2_nullcond_call_explicit_type_arguments) {
    auto r = runAot(kExplicitTypeArgumentsClassParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "80\n");
}

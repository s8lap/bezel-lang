// Regression tests for the #4813 follow-up (gate FAIL on r21-c26 @ dcb17d57,
// `generic_method_same_name_tparam_4813.null_conditional_call_spec_14_1`):
// the explicit method type arguments of a NULL-CONDITIONAL call
// `recv?.Method<A>(...)` were never resolved.
//
// MECHANISM. `resolveTypesInExpr` (TypeResolution_Names.cpp) resolved the
// `typeArgs` of a MemberAccessExpr callee (#509) and an IdentifierExpr callee
// (#520) but not of a NullCondMemberExpr callee, although the parser fills
// all three (#1278). Every consumer reads the list through
// `ctx_.resolvedType(ta)`, which returned null for the `?.` form, so typeCall
// marked the call as having an explicit list yet bound nothing. Argument
// inference used to backfill the binding (silently: `b?.Ret<long>(6)` bound
// `int`); once #4935 (e8bc97fe) correctly stopped inference when an explicit
// list is present (§14.5), `b?.Ret<int>(6)` was typed as the callee's bare
// `T` — rejected against the caller's same-named `T` in `Go<T>`, and
// mis-lowered (printed an empty line) when bound to an inferred local.
//
// AUTHORITY (spec v0.307). §14.5: "When a full explicit list is supplied, no
// inference runs" — the written arguments ARE the instantiation. §7.5: `x?.M`
// performs the ordinary access when `x` is non-null, and its result type is
// the member's result type made nullable. So `b?.Ret<int>(6)` is `int?`, and
// `b?.Ret<long>(6)` is `long?` (not convertible to `int?`, §5.1).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

bool rejectedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ═════════ Accept rows (value oracle) ════════════════════════════════════

// The gate-failure shape: `b?.Ret<int>(6)` into `int?` inside a generic
// caller whose parameter is also named `T`. Pre-fix: rejected ("`T` is a type
// parameter, and a generic body converts …").
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_int_into_int_nullable_in_same_name_caller_spec_14_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Util
  public static method Go<T>(v: T, b: Box?): string
    var r: int? := b?.Ret<int>(6);
    return $"{r ?? 0}";
  end Go;
end Util;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    Console.WriteLine(Util.Go<string>("q", b));
    Console.WriteLine(Util.Go<string>("q", n));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n0\n"));
}

// The same call bound to an INFERRED local (`var r := b?.Ret<int>(6)`).
// Pre-fix this was accepted but typed as the bare `T`, and printed an empty
// line instead of `6` (silent wrong value).
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_int_into_inferred_local_value_spec_14_5_7_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Util
  public static method Go<T>(v: T, b: Box?): string
    var r := b?.Ret<int>(6);
    var s: int? := r;
    return $"{r ?? 0}/{s ?? 0}";
  end Go;
end Util;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    Console.WriteLine(Util.Go<string>("q", b));
    Console.WriteLine(Util.Go<string>("q", n));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6/6\n0/0\n"));
}

// Explicit `<long>` binds `long`, not the literal's inferred `int`: a value
// above the `int` range round-trips, and the result widens into `long?`.
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_long_binds_long_spec_14_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Util
  public static method Go<T>(v: T, b: Box?): string
    var r: long? := b?.Ret<long>(6);
    var w: long? := b?.Ret<long>(9000000000L);
    return $"{r ?? 0L}|{w ?? 0L}";
  end Go;
end Util;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    Console.WriteLine(Util.Go<string>("q", b));
    Console.WriteLine(Util.Go<string>("q", n));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6|9000000000\n0|0\n"));
}

// An explicit argument that is the CALLER's `T` (`b?.Ret<T>(v)`) and a
// reference-typed explicit argument (`b?.Ret<string>("z")`).
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_caller_t_and_string_spec_14_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Util
  public static method Go<T>(v: T, b: Box?): string
    var r := b?.Ret<T>(v);
    var s: string? := b?.Ret<string>("z");
    if r is null then
      return $"nil|{s ?? "-"}";
    end if;
    return $"{r}|{s ?? "-"}";
  end Go;
end Util;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    Console.WriteLine(Util.Go<string>("q", b));
    Console.WriteLine(Util.Go<string>("q", n));
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("q|z\nnil|-\n"));
}

// Issue #5027 N1: pin the silent WRONG VALUE directly in a NON-generic
// caller. Every accept row above runs inside a generic `Go<T>` — there the
// pre-fix failure was a compile REJECT (the #4768 "`T` is a type parameter"
// diagnostic), not a wrong value. Outside a generic caller there is no
// same-named type parameter to collide with, so pre-fix the unresolved
// explicit list let argument inference silently backfill the binding from
// the (unresolved) call, and `var r := b?.Ret<int>(6)` was typed as the
// bare, unsubstituted `T` and mis-lowered — it compiled and printed an
// EMPTY LINE instead of `6`.
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_int_into_inferred_local_non_generic_caller_spec_14_5_7_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    var r := b?.Ret<int>(6);
    var s := n?.Ret<int>(6);
    Console.WriteLine($"{r ?? 0}");
    Console.WriteLine($"{s ?? 0}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n0\n"));
}

// String variant of the same non-generic-caller shape.
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           explicit_string_into_inferred_local_non_generic_caller_spec_14_5_7_5) {
    auto r = runAot(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var n: Box? := null;
    var r := b?.Ret<string>("x");
    var s := n?.Ret<string>("x");
    Console.WriteLine(r ?? "-");
    Console.WriteLine(s ?? "-");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x\n-\n"));
}

// ═════════ Reject rows ═══════════════════════════════════════════════════

// Control: the caller's own `T` returned into `int?` is still rejected by the
// #4768 arm (§5.1 / §14.2) — the fix must not make `Ret<T>` look concrete.
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           reject_caller_t_into_int_nullable_spec_5_1_14_2) {
    BEZEL_EXPECT_TRUE(rejectedWith(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Util
  public static method Go<T>(v: T, b: Box?): string
    var r: int? := b?.Ret<T>(v);
    return $"{r ?? 0}";
  end Go;
end Util;
public class Program
  public static method Main()
    var b: Box? := new Box();
    Console.WriteLine(Util.Go<string>("q", b));
  end Main;
end Program;
)BZ", "`T` is a type parameter, and a generic body converts"));
}

// `b?.Ret<long>(6)` is `long?`, which does not narrow into `int?` (§5.1).
// Pre-fix the unresolved list let inference bind `int` from the literal and
// the program was ACCEPTED (non-generic caller, so no type-parameter arm).
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           reject_explicit_long_into_int_nullable_spec_5_1_14_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var r: int? := b?.Ret<long>(6);
    Console.WriteLine($"{r ?? 0}");
  end Main;
end Program;
)BZ", "the initializer for `r` has a type that is not implicitly convertible"));
}

// The explicit list also drives the ARGUMENT check (#4325): `"z"` is not an
// `int`, so `b?.Ret<int>("z")` is rejected at the argument.
BEZEL_TEST(nullcond_explicit_type_args_resolved_4813,
           reject_argument_against_explicit_int_spec_5_1_14_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(R"BZ(module Conf;
public class Box
  public method Ret<T>(x: T): T
    return x;
  end Ret;
end Box;
public class Program
  public static method Main()
    var b: Box? := new Box();
    var r: int? := b?.Ret<int>("z");
    Console.WriteLine($"{r ?? 0}");
  end Main;
end Program;
)BZ", "the argument `x` to call to `Ret` has a type that is not implicitly convertible"));
}

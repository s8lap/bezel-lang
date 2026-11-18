// Spec-derived regression tests for issue #8408 — an interface-typed argument
// accepted at a class-typed parameter.
//
// THE RULE. Spec §5.1 "Implicit conversions" (v0.357) lists the implicit
// reference conversions in a CLOSED list:
//
//   - **identity**: every type converts to itself;
//   - **class to base class**, transitively, up to `Object`;
//   - **any type to `Object`**;
//   - **a type to an interface it implements or conforms to**;
//   - **interface to extended interface**, directly or transitively;
//   - **a type parameter to an interface or class its constraints guarantee
//     conformance to**;
//   - the **variance-induced conversions** of §12.9.
//
// There is NO row from an INTERFACE to a CLASS. A downcast needs §12.7's
// checked spellings (`is` narrows, `as` converts and yields null).
//
// WHAT WAS BROKEN. `ExprTC::checkAssignable` (src/sema/ExprTypecheck.cpp) —
// the §5.1 conversion check every written-declared-type position runs
// (`ExprTC::checkArgsAgainstParams` calls it for each call argument) — decides
// a class↔class pair through a DECLARATION-KEYED check that needs BOTH
// `from->declaration` and `to->declaration`. A built-in class target
// (`Exception`, `IOException`, `StringBuilder`, …) carries no TypeSymbol, so
// that check never saw the pair; every class-target arm in the same function
// is keyed on the SOURCE being a class, so a USER interface source matched
// none of them; and the function's final "Mixed / generic-parameter /
// null-literal cases: skip" accepted what was left.
//
// MEASURED on the base (r30-c1 @ e69d8b050): the issue's repro COMPILED,
// exited 0, and printed `Log: 'hello'` and then `Log: ''` — the second
// argument is not an `Exception` at all, and the `Message` read went through
// a vtable the object does not have. The same shape was accepted at a local
// initializer, at a `StringBuilder` parameter, at an `IOException` parameter,
// and (with a built-in interface source) at an `Exception` and at a user-class
// parameter.
//
// Every row below is on `runAot` and is RED on the base: each program compiles
// and RUNS there, so `r.ran` is true and no diagnostic exists. A reject row
// asserts: did not run, rejected with a real source location, and the
// diagnostic names the §5.1 rule and the §12.7 / §17.4 remedy.
//
// The ADMITTING TWINS of these shapes — the upcasts §5.1 does list (a class to
// a base class, a class to an interface it implements, an interface to
// `Object`, an interface to an interface it extends, a type parameter to its
// bound) — are base-green by construction and are NOT gtest rows; they are in
// the lane's `corpus-candidates/` directory with the §5.1 sentence that
// decides each one (lane contract §7, RUN.md "Base-green rows go to the
// CORPUS, not gtest").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The §5.1 rejection wording this fix introduces, and the two §5.1 clauses it
// rests on. `reportNotConvertible` prefixes every reason with "the <what> has
// a type that is not implicitly convertible to the target type (§5.1): ".
constexpr const char* kRuleReason =
    "an interface type is not implicitly convertible to a class type";
constexpr const char* kRuleClause =
    "§5.1 lists no conversion from an interface to a class";

// The remedy the diagnostic names: §12.7's `is` and §17.4's `as`, applied to
// the TARGET class.
std::string remedyFor(const char* targetClass) {
    return std::string("test the runtime type with `is ") + targetClass +
           "` (§12.7) or convert with `as " + targetClass + "` (§17.4)";
}

void expectInterfaceToClassRejected(const JitResult& r, const char* targetClass) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleReason));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRuleClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, remedyFor(targetClass).c_str()));
}

}  // namespace

// REJECT — THE ISSUE'S REPRO (verbatim, including its `module F18;` and its
// `Program.Log(i)` / `Program.Log(j)`): an interface-typed value at a class-
// typed parameter. §5.1 lists no interface→class conversion.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           issue_repro_interface_argument_at_exception_parameter_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module F18;

public interface IMark
end IMark;

public class A: Exception, IMark
  public constructor(m: string) base(m); end constructor;
end A;

public class NotAnException: IMark
end NotAnException;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: '{ex.Message}'");
  end Log;

  public static method Main()
    using var a := new A("hello");
    var i: IMark := a;
    Program.Log(i);
    using var n := new NotAnException();
    var j: IMark := n;
    Program.Log(j);
  end Main;
end Program;
)bzl");
    expectInterfaceToClassRejected(r, "Exception");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — BOTH call sites of the issue's repro are diagnosed. §5.1's rule
// governs each argument position independently, so the second call is not
// excused by the first being rejected. (On the base the program compiled and
// printed `Log: 'hello'` then `Log: ''`.)
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           issue_repro_reports_both_call_sites_spec_5_1) {
    auto r = runAot(R"bzl(module F18;
public interface IMark
end IMark;
public class A: Exception, IMark
  public constructor(m: string) base(m); end constructor;
end A;
public class NotAnException: IMark
end NotAnException;
public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: '{ex.Message}'");
  end Log;
  public static method Main()
    using var a := new A("hello");
    var i: IMark := a;
    Program.Log(i);
    using var n := new NotAnException();
    var j: IMark := n;
    Program.Log(j);
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kRuleReason), 2);
}

// REJECT — the issue's SECOND call alone: the argument is not an `Exception`
// at all, and the callee read its `Message` as the empty string.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           issue_repro_non_exception_argument_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module P8408;
public interface IMark
end IMark;
public class NotAnException: IMark
end NotAnException;
public class Program
  static method Log(ex: Exception)
    Console.WriteLine($"Log: '{ex.Message}'");
  end Log;
  public static method Main()
    using var n := new NotAnException();
    var j: IMark := n;
    Program.Log(j);
    Console.WriteLine("RAN");
    return 0;
  end Main;
end Program;
)bzl");
    expectInterfaceToClassRejected(r, "Exception");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — the same shape at a LOCAL initializer, not only at a call
// argument: `checkAssignable` is the ONE §5.1 check every written-declared-
// type position runs, so the local is rejected too.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           interface_local_at_exception_storage_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module P8408; public interface IMark end IMark; "
        "public class A: IMark end A; "
        "public class P "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    var i: IMark := a; "
        "    var e: Exception := i; "
        "    Console.WriteLine(\"RAN\"); return 0; "
        "  end Main; end P;"));
    expectInterfaceToClassRejected(r, "Exception");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — a built-in exception target that is NOT `Exception`: the rule is
// the §5.1 list, not the `Exception` class.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           interface_argument_at_ioexception_parameter_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module P8408; public interface IMark end IMark; "
        "public class A: IMark end A; "
        "public class P "
        "  public static method Log(ex: IOException) "
        "    Console.WriteLine(\"RAN\"); "
        "  end Log; "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    var i: IMark := a; "
        "    Log(i); "
        "    return 0; "
        "  end Main; end P;"));
    expectInterfaceToClassRejected(r, "IOException");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — a built-in class target that is not an exception at all: the §5.1
// list names no interface→class conversion for any class.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           interface_argument_at_builtin_class_parameter_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module P8408; public interface IMark end IMark; "
        "public class A: IMark end A; "
        "public class P "
        "  public static method Log(sb: StringBuilder) "
        "    Console.WriteLine(\"RAN\"); "
        "  end Log; "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    var i: IMark := a; "
        "    Log(i); "
        "    return 0; "
        "  end Main; end P;"));
    expectInterfaceToClassRejected(r, "StringBuilder");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — a BUILT-IN interface source. §5.1's list is a list of CONVERSIONS
// between types, not of user declarations: `IEnumerable<T>` is an interface
// type exactly as `IMark` is, and no row converts one to a class.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           builtin_interface_argument_at_exception_parameter_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module P8408; "
        "public class P "
        "  public static method Log(ex: Exception) "
        "    Console.WriteLine(\"RAN\"); "
        "  end Log; "
        "  public static method Main(): int "
        "    var arr: Array<int> := [1, 2, 3]; "
        "    var xs: IEnumerable<int> := arr; "
        "    Log(xs); "
        "    return 0; "
        "  end Main; end P;"));
    expectInterfaceToClassRejected(r, "Exception");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — `IDisposable` at a class-typed parameter. §5.1 names `IDisposable`
// only as a TARGET ("`IDisposable` is reached only from a class, an interface
// that extends it, or a type parameter whose constraints guarantee it"), and
// `IDisposable` is itself an interface — so it is not convertible to a class
// either.
BEZEL_TEST(sema_interface_to_class_not_implicit_8408,
           idisposable_argument_at_exception_parameter_rejected_spec_5_1) {
    auto r = runAot(std::string(
        "module P8408; "
        "public class A destructor Console.WriteLine(\"D\"); end destructor; "
        "end A; "
        "public class P "
        "  public static method Log(ex: Exception) "
        "    Console.WriteLine(\"RAN\"); "
        "  end Log; "
        "  public static method Main(): int "
        "    using var a := new A(); "
        "    var d: IDisposable := a; "
        "    Log(d); "
        "    return 0; "
        "  end Main; end P;"));
    expectInterfaceToClassRejected(r, "Exception");
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

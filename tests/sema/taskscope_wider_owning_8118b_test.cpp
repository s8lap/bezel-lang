// fix-forward #8118 (stdlib §6.10, v0.355; #5657): a TaskScope placed into an
// OWNING position of a WIDER static type — reached by a language spec §5.1
// reference conversion — is a compile error naming the §6.10 rule.
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Standard_Library.md (§6.10, v0.355): "Consumption is `await
//   using` only, and that binding is the scope's sole owner — a `TaskScope` is
//   minted only as the initializer of an `await using var` binding whose
//   static type is `TaskScope` ... Every other owning position is a compile
//   error naming this rule, at the declaration that spells the position or at
//   the expression that would place a `TaskScope` in one, ... and an owning
//   position of a wider static type reached by a language spec §5.1 reference
//   conversion (an `owned Object` return, an `owned IAsyncDisposable`
//   parameter, an `await using var` declared at `Object` or at an interface).
//   Borrows are otherwise unrestricted — a plain parameter at `TaskScope`, at
//   `Object` or at an interface, a pattern binding, a child body's
//   self-capture (below)".
//
// THE BUG. Before #8118, `owned Object` was misread as "a primitive value
// type" by the §28.1 type requirement, so the corpus case l6_10_0009 (`Make():
// owned Object return new TaskScope();`) was rejected — but only by ACCIDENT,
// with a false message. #8118 rightly admitted `owned Object`, and the program
// then compiled: the §6.10 rule itself was never enforced at a wider owning
// position. The `owned IAsyncDisposable` parameter (l6_10_0011) and the `await
// using var` at `Object` (l6_10_0012) compiled before #8118 too.
//
// THE FIX. The ownership pass reports a TaskScope-typed expression placed at
// an owning position (an `owned` return slot, an `owned` parameter, an `owned`
// field, an owning inline-array slot), and checkTaskScopeConsumption reports
// an `await using var` declared at a type other than `TaskScope` over a
// TaskScope initializer.
//
// THE ORACLE. Reject rows: `runAot`, the driver rejects at compile time with a
// real location, and the SPECIFIC §6.10 text is present. Accept rows (the
// borrow controls the rule text names, and the same owning positions with a
// non-TaskScope value): `runAot`, the program compiles, RUNS, and prints.
//
// per stdlib §6.10, spec §5.1, §28.1, §28.12, v0.355

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kPlaced = "a TaskScope may not be placed in ";
const char* kWiderAwaitUsing =
    "is an `await using var` declared at a wider static type than `TaskScope`";

void expectRejected(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kPlaced));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, position));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §6.10"));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

const char* kMain =
    "public class App "
    "  public static method Main(): int "
    "    Console.WriteLine(\"main\"); "
    "    return 0; "
    "  end Main; "
    "end App; ";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the `owned` return slot at a wider static type.
// ---------------------------------------------------------------------------

// The corpus case l6_10_0009 verbatim (plus a Main): the §6.10 rule text's own
// first example, "an `owned Object` return".
BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_return_rejected_stdlib_6_10_l6_10_0009) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Factory "
        "  public static method Make(): owned Object "
        "    return new TaskScope(); "
        "  end Make; "
        "end Factory; " + std::string(kMain));
    expectRejected(r, "an `owned` return slot");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_expression_body_return_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Factory "
        "  public static method Make(): owned Object => new TaskScope(); "
        "end Factory; " + std::string(kMain));
    expectRejected(r, "an `owned` return slot");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_iasyncdisposable_return_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Factory "
        "  public static method Make(): owned IAsyncDisposable "
        "    return new TaskScope(); "
        "  end Make; "
        "end Factory; " + std::string(kMain));
    expectRejected(r, "an `owned` return slot");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_nullable_object_return_on_one_path_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Factory "
        "  public static method Make(b: bool): owned Object? "
        "    if b then "
        "      return new TaskScope(); "
        "    end; "
        "    return null; "
        "  end Make; "
        "end Factory; " + std::string(kMain));
    expectRejected(r, "an `owned` return slot");
}

// ---------------------------------------------------------------------------
// REJECT — the `owned` parameter at a wider static type.
// ---------------------------------------------------------------------------

// The corpus case l6_10_0011 (plus a Main): "an `owned IAsyncDisposable`
// parameter".
BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_iasyncdisposable_parameter_rejected_stdlib_6_10_l6_10_0011) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Take(owned d: IAsyncDisposable) "
        "  end Take; "
        "  public method Run() "
        "    Take(new TaskScope()); "
        "  end Run; "
        "end Program; " + std::string(kMain));
    expectRejected(r, "an `owned` parameter");
}

// Inside a loop body: the dataflow's fixpoint passes are diagnostics-
// suppressed, so the call site is reported exactly ONCE.
BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_parameter_in_loop_rejected_once_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Take(owned d: Object) "
        "  end Take; "
        "  public method Run() "
        "    var i := 0; "
        "    while i < 3 do "
        "      Take(new TaskScope()); "
        "      i := i + 1; "
        "    end while; "
        "  end Run; "
        "end Program; " + std::string(kMain));
    expectRejected(r, "an `owned` parameter");
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kPlaced), 1);
}

// A named argument binds the `owned` parameter out of source order (§22.5).
BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_named_argument_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Take(x: int, owned d: Object) "
        "  end Take; "
        "  public method Run() "
        "    Take(d := new TaskScope(), x := 1); "
        "  end Run; "
        "end Program; " + std::string(kMain));
    expectRejected(r, "an `owned` parameter");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_constructor_parameter_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Box "
        "  private owned _o: Object?; "
        "  public constructor(owned o: Object) "
        "    _o := o; "
        "  end constructor; "
        "end Box; "
        "public class Program "
        "  public method Run() "
        "    using var b := new Box(new TaskScope()); "
        "  end Run; "
        "end Program; " + std::string(kMain));
    expectRejected(r, "an `owned` parameter");
}

// ---------------------------------------------------------------------------
// REJECT — the `owned` field at a wider static type.
// ---------------------------------------------------------------------------

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_field_initializer_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Holder "
        "  private owned _o: Object? := new TaskScope(); "
        "  public constructor() "
        "  end constructor; "
        "end Holder; " + std::string(kMain));
    expectRejected(r, "an `owned` field");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_field_store_in_constructor_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Holder "
        "  private owned _o: Object?; "
        "  public constructor() "
        "    _o := new TaskScope(); "
        "  end constructor; "
        "end Holder; " + std::string(kMain));
    expectRejected(r, "an `owned` field");
}

BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_field_store_in_method_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Holder "
        "  private owned _o: Object?; "
        "  public constructor() "
        "    _o := null; "
        "  end constructor; "
        "  public method Set() "
        "    this._o := new TaskScope(); "
        "  end Set; "
        "end Holder; " + std::string(kMain));
    expectRejected(r, "an `owned` field");
}

// ---------------------------------------------------------------------------
// REJECT — the `await using var` declared at a wider static type.
// ---------------------------------------------------------------------------

// The corpus case l6_10_0012 (plus a Main): "an `await using var` declared at
// `Object`". It compiled before this fix: the declared type was ignored and the
// narrowed `scope.Join()` satisfied the missing-Join check.
BEZEL_TEST(taskscope_wider_owning_8118b,
           await_using_var_at_object_rejected_stdlib_6_10_l6_10_0012) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static async method Run(): Task "
        "    await using var scope: Object := new TaskScope(); "
        "    if scope is TaskScope then "
        "      await scope.Join(); "
        "    end; "
        "  end Run; "
        "end Program; " + std::string(kMain));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWiderAwaitUsing));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §6.10"));
}

// "... or at an interface". Before the fix this was rejected only by the
// missing-Join check (there is no `Join` on `IAsyncDisposable`); the wider-type
// diagnostic now replaces that one.
BEZEL_TEST(taskscope_wider_owning_8118b,
           await_using_var_at_interface_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static async method Run(): Task "
        "    await using var scope: IAsyncDisposable := new TaskScope(); "
        "  end Run; "
        "end Program; " + std::string(kMain));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWiderAwaitUsing));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, ".Join()` is missing"));
}

// ---------------------------------------------------------------------------
// ACCEPT — the borrow controls §6.10 names, and the TaskScope-typed binding.
// ---------------------------------------------------------------------------

// "a plain parameter ... at `Object`" — a borrow, unrestricted.
BEZEL_TEST(taskscope_wider_owning_8118b,
           borrow_parameter_at_object_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Log(x: Object) "
        "    Console.WriteLine(\"logged\"); "
        "  end Log; "
        "  public static async method Main(): Task "
        "    await using var scope := new TaskScope(); "
        "    Log(scope); "
        "    await scope.Join(); "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "logged\n");
}

// "... or at an interface" — a borrow, unrestricted.
BEZEL_TEST(taskscope_wider_owning_8118b,
           borrow_parameter_at_interface_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Log(x: IAsyncDisposable) "
        "    Console.WriteLine(\"logged\"); "
        "  end Log; "
        "  public static async method Main(): Task "
        "    await using var scope := new TaskScope(); "
        "    Log(scope); "
        "    await scope.Join(); "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "logged\n");
}

// The one admitted production, with the static type spelled explicitly — the
// wider-type arm keys off a declared type OTHER than `TaskScope`.
BEZEL_TEST(taskscope_wider_owning_8118b,
           await_using_var_declared_at_taskscope_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static async method Main(): Task "
        "    await using var scope: TaskScope := new TaskScope(); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"joined\"); "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "joined\n");
}

// The same owning positions with a non-TaskScope value stay admitted: the new
// rejection is keyed on the VALUE's static type, not on the position.
BEZEL_TEST(taskscope_wider_owning_8118b,
           owned_object_positions_with_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; "
        "public class Res "
        "  public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Res.n := Res.n + 1; end destructor; "
        "end Res; "
        "public class Holder "
        "  private owned _o: Object? := new Res(); "
        "  public constructor() "
        "  end constructor; "
        "end Holder; "
        "public class App "
        "  public static method Take(owned d: Object): int "
        "    return 1; "
        "  end Take; "
        "  public static method Make(): owned Object "
        "    return new Res(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new Res())); "
        "    do "
        "      var o := Make(); "
        "    end; "
        "    do "
        "      using var h := new Holder(); "
        "    end; "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n3\n");
}

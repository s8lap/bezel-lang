// fix #8160 (stdlib §6.10, v0.355; #5657): a TaskScope is still admitted at
// six OWNING positions after #8118's fix-forward — an `owned` function-type
// slot, an `out owned` parameter, an `owned` choice payload, an
// `OwningTask<T>` result slot, a `static` method's class-typed return slot,
// and an `as Object` cast into an `owned` parameter.
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Standard_Library.md (§6.10, v0.355): "Consumption is `await
//   using` only, and that binding is the scope's sole owner — a `TaskScope` is
//   minted only as the initializer of an `await using var` binding whose
//   static type is `TaskScope` ... Every other owning position is a compile
//   error naming this rule, at the declaration that spells the position or at
//   the expression that would place a `TaskScope` in one, `TaskScope?` reading
//   as `TaskScope` throughout: ... any position language spec §28.1 admits
//   `owned` at — a parameter (`out` and lambda parameters included), a return
//   slot, a field, a choice payload, a function-type slot, an owning
//   inline-array slot; ... and an owning position of a wider static type
//   reached by a language spec §5.1 reference conversion (an `owned Object`
//   return, an `owned IAsyncDisposable` parameter, an `await using var`
//   declared at `Object` or at an interface). Borrows are otherwise
//   unrestricted — a plain parameter at `TaskScope`, at `Object` or at an
//   interface, a pattern binding, a child body's self-capture (below)".
//
// THE BUG. #8118 added `reportTaskScopeInOwningPosition` (src/ownership), which
// reports an expression whose OWN static type is `TaskScope` at an owning
// position, and hooked it at the `owned` return / parameter / field /
// owning-inline-array-slot positions. Six owning positions were still
// admitted (MEASURED by the R39 review r39-rev-8118b on #8118's fix-forward
// 03fb7b90, reproduced here on the base):
//
//   1. an `owned` FUNCTION-TYPE slot — `var f: () -> owned Object := lambda()
//      => new TaskScope();` — no hook at the lambda's return;
//   2. an `out owned` PARAMETER — `Make(out owned o: Object)` with `o := new
//      TaskScope();` — no hook at the owned-OUT store;
//   3. an `owned` CHOICE PAYLOAD — `Slot.Full(new TaskScope())` — the #8118
//      comment on the recordMovesInArgs hook CLAIMED this was covered and it
//      was not: the §28.1 per-instantiation erasure turns a payload declared
//      `owned o: Object` into a non-sink, so the sink arm never ran for it;
//   4. an `OwningTask<T>` RESULT slot — `async method Make(): OwningTask<Object>
//      return new TaskScope();` — no hook at the async return;
//   5. a `static` method's CLASS-TYPED RETURN slot — `static method Make():
//      Object return new TaskScope();` — §28.3.6 requires the `owned` marker
//      there, so the slot IS an owning position, and §28.3.6's declaration-site
//      rule does not reach `Object` (MEASURED: `static method Make(): Item` IS
//      rejected at the declaration, `static method Make(): Object` was not);
//   6. an `as Object` CAST into an `owned` parameter — `Take(new TaskScope() as
//      Object)` — the cast widens the expression's static type, but the VALUE
//      placed in the owning position is the TaskScope.
//
// THE FIX. The ONE §6.10 checker — `reportTaskScopeInOwningPosition` — is
// extended: it now reads the OPERAND's static type through an `as` cast (a
// §5.1 reference conversion), and the six positions are hooked into it. There
// is no second checker.
//
// THE ORACLE. Reject rows: `runAot`, the driver rejects at compile time with a
// real location, and the SPECIFIC §6.10 text is present. Accept rows (the
// borrow controls the rule text names, and each reject shape's admitting twin
// — the same position with a legal value): `runAot`, the program compiles,
// RUNS, and prints.
//
// per stdlib §6.10, spec §5.1, §28.1, §28.3.6, §30.3, v0.355

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

// The accept oracle for a program whose STDOUT is not this lane's business:
// it compiled (a compile-time rejection leaves `ran` false) and it ran to
// completion. Used for the one control whose output is pinned by a
// PRE-EXISTING defect (issue #8000 — a `case` over a `TaskScope` prints
// neither arm), so this lane pins no expectation on it.
void expectRan(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A TaskScope and a legal value, so the owning positions below can be spelled
// twice: once with the TaskScope (must be rejected) and once with the Item
// (must be admitted).
const char* kItem =
    "public class Item "
    "  public constructor() "
    "  end constructor; "
    "end Item; ";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT 1 — the `owned` FUNCTION-TYPE slot (a lambda's `owned` return).
// ---------------------------------------------------------------------------

// §6.10 names "a function-type slot" among the positions §28.1 admits `owned`
// at. The lambda's target function type declares an `owned` return, so the
// caller receives a value it must dispose — and a TaskScope's discharge is
// §28.12's implicit synchronous path, which cannot await the drain of its
// children.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_function_type_slot_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Run() "
        "    var f: () -> owned Object := lambda() => new TaskScope(); "
        "    using var o := f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `owned` function-type slot");
}

// The block-bodied lambda twin of the row above: the `owned` return slot is
// the lambda's own, so the `return` statement inside the body is the
// expression that would place the TaskScope in it.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_function_type_slot_block_body_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Run() "
        "    var f: () -> owned Object := lambda() => do "
        "      return new TaskScope(); "
        "    end; "
        "    using var o := f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `owned` function-type slot");
}

// ---------------------------------------------------------------------------
// REJECT 2 — the `out owned` PARAMETER (the owned-OUT direction).
// ---------------------------------------------------------------------------

// §6.10 names "a parameter (`out` and lambda parameters included)" among the
// positions it rejects a TaskScope in. The callee produces the value and the
// CALLER becomes responsible for discharging it — on the ordinary synchronous
// path, which cannot await the drain of the scope's children.
BEZEL_TEST(taskscope_owning_positions_8160,
           out_owned_parameter_store_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Make(out owned o: Object) "
        "    o := new TaskScope(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `out owned` parameter");
}

// ---------------------------------------------------------------------------
// REJECT 3 — the `owned` CHOICE PAYLOAD.
// ---------------------------------------------------------------------------

// §6.10 names "a choice payload" among the positions it rejects a TaskScope
// in. The payload slot is DECLARED `owned`, which is what the rule reads; the
// §28.1 per-instantiation erasure (the marker is vacuous at `Object`) answers
// a different question — whether the VALUE in the slot carries a disposal
// obligation — and must not switch the rule off.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_choice_payload_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public choice Slot "
        "  Full(owned o: Object), "
        "  Empty; "
        "end Slot; "
        "public class Program "
        "  public static method Run() "
        "    using var s := Slot.Full(new TaskScope()); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `owned` choice payload");
}

// The same payload, the construction passed as the argument of an `owned`
// parameter — the payload position is the same one, reached through one more
// call.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_choice_payload_through_owned_argument_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public choice Slot "
        "  Full(owned o: Object), "
        "  Empty; "
        "end Slot; "
        "public class Program "
        "  public static method Keep(owned s: Slot) "
        "  end Keep; "
        "  public static method Run() "
        "    Keep(Slot.Full(new TaskScope())); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `owned` choice payload");
}

// ---------------------------------------------------------------------------
// REJECT 4 — the `OwningTask<T>` RESULT slot (an async method).
// ---------------------------------------------------------------------------

// §30.3 / §30.10: `OwningTask<T>` OWNS its result — the task disposes an
// unconsumed owned result — so the result slot is an owning position, and its
// static type (`Object` at `OwningTask<Object>`) is WIDER than `TaskScope`:
// §6.10's last clause rejects "an owning position of a wider static type
// reached by a language spec §5.1 reference conversion".
BEZEL_TEST(taskscope_owning_positions_8160,
           owning_task_result_slot_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static async method Make(): OwningTask<Object> "
        "    return new TaskScope(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `OwningTask<T>` result slot");
}

// ---------------------------------------------------------------------------
// REJECT 5 — a `static` method's class-typed RETURN slot.
// ---------------------------------------------------------------------------

// §28.3.6 (v0.355): "A `static` method or `operator` declaration ... whose
// declared return type is a class or interface type is a compile error unless
// the declared type itself announces the classification" — so the `owned`
// marker is REQUIRED at a `static` method's class-typed return, and the slot
// is an owning position of the §6.10 kind ("a return slot").
//
// The declaration-site rule does not reach `Object` — MEASURED: `static method
// Make(): Item` is rejected at the declaration ("static method `Make` returns
// class/interface type `Item` without `owned`"), `static method Make(): Object`
// was admitted — because that rule keys on a resolved TypeDecl and `Object` is
// the builtin root class (§8.1). The §6.10 expression-side rule is what
// closes the position; it is keyed on `Object` alone, so the two rules never
// report the same slot twice.
BEZEL_TEST(taskscope_owning_positions_8160,
           static_object_return_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Make(): Object "
        "    return new TaskScope(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "a `static` method's return slot");
}

// The `=>` twin of the row above.
BEZEL_TEST(taskscope_owning_positions_8160,
           static_object_return_expression_body_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Make(): Object => new TaskScope(); "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "a `static` method's return slot");
}

// ---------------------------------------------------------------------------
// REJECT 6 — the `as Object` CAST into an `owned` parameter.
// ---------------------------------------------------------------------------

// §6.10 rejects "an owning position of a wider static type reached by a
// language spec §5.1 reference conversion". `new TaskScope() as Object` HAS
// the static type `Object` — the cast widens it — but the VALUE placed in the
// owning position is the TaskScope, so the rule reads the OPERAND's static
// type.
BEZEL_TEST(taskscope_owning_positions_8160,
           as_object_cast_into_owned_parameter_rejected_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Take(owned o: Object) "
        "  end Take; "
        "  public static method Run() "
        "    Take(new TaskScope() as Object); "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRejected(r, "an `owned` parameter");
}

// ---------------------------------------------------------------------------
// ACCEPT — the ADMITTING TWINS: each reject shape with a legal value.
// ---------------------------------------------------------------------------

// The `owned` function-type slot with a legal value.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_function_type_slot_with_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; " + std::string(kItem) +
        "public class Program "
        "  public static method Run() "
        "    var f: () -> owned Object := lambda() => new Item(); "
        "    using var o := f(); "
        "    Console.WriteLine(\"ok\"); "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(); "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "ok\nmain\n");
}

// The `out owned` parameter with a legal value.
BEZEL_TEST(taskscope_owning_positions_8160,
           out_owned_parameter_store_with_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; " + std::string(kItem) +
        "public class Program "
        "  public static method Make(out owned o: Object) "
        "    o := new Item(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "main\n");
}

// The `owned` choice payload with a legal value (the review's U08c control).
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_choice_payload_with_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; " + std::string(kItem) +
        "public choice Slot "
        "  Full(owned o: Object), "
        "  Empty; "
        "end Slot; "
        "public class Program "
        "  public static method Run() "
        "    using var s := Slot.Full(new Item()); "
        "    Console.WriteLine(\"ok\"); "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(); "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "ok\nmain\n");
}

// The `OwningTask<T>` result slot with a legal value.
BEZEL_TEST(taskscope_owning_positions_8160,
           owning_task_result_slot_with_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; " + std::string(kItem) +
        "public class Program "
        "  public static async method Make(): OwningTask<Object> "
        "    return new Item(); "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "main\n");
}

// The `as Object` cast into an `owned` parameter with a legal value.
BEZEL_TEST(taskscope_owning_positions_8160,
           as_object_cast_into_owned_parameter_with_other_class_admitted) {
    JitResult r = runAot(
        "module M; " + std::string(kItem) +
        "public class Program "
        "  public static method Take(owned o: Object) "
        "  end Take; "
        "  public static method Run() "
        "    Take(new Item() as Object); "
        "    Console.WriteLine(\"ok\"); "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(); "
        "    Console.WriteLine(\"main\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "ok\nmain\n");
}

// ---------------------------------------------------------------------------
// ACCEPT — the BORROW CONTROLS §6.10 names: "Borrows are otherwise
// unrestricted — a plain parameter at `TaskScope`, at `Object` or at an
// interface, a pattern binding, a child body's self-capture".
// ---------------------------------------------------------------------------

// "a plain parameter at `TaskScope`" — the borrow reaches a plain `Object`
// parameter.
BEZEL_TEST(taskscope_owning_positions_8160,
           borrow_parameter_at_taskscope_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Log(x: Object) "
        "    Console.WriteLine(\"logged\"); "
        "  end Log; "
        "  public static method Pass(s: TaskScope) "
        "    Log(s); "
        "  end Pass; "
        "  public static async method Main(): Task "
        "    await using var scope := new TaskScope(); "
        "    Pass(scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "logged\ndone\n");
}

// "a plain parameter ... at an interface".
BEZEL_TEST(taskscope_owning_positions_8160,
           borrow_parameter_at_interface_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Peek(d: IAsyncDisposable) "
        "    Console.WriteLine(\"peek\"); "
        "  end Peek; "
        "  public static async method Main(): Task "
        "    await using var scope: TaskScope := new TaskScope(); "
        "    Peek(scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "peek\ndone\n");
}

// "a child body's self-capture" — the child body borrows the scope itself.
BEZEL_TEST(taskscope_owning_positions_8160,
           child_self_capture_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Use(s: TaskScope) "
        "    Console.WriteLine(\"use\"); "
        "  end Use; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      Use(scope); "
        "      var tok := scope.Token; "
        "      Console.WriteLine(\"child\"); "
        "    end); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "use\nchild\ndone\n");
}

// An instance method's borrows, positional and named (§22.5).
BEZEL_TEST(taskscope_owning_positions_8160,
           instance_method_borrows_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Worker "
        "  public constructor() "
        "  end constructor; "
        "  public method Attach(s: TaskScope, o: Object) "
        "    Console.WriteLine(\"attach\"); "
        "  end Attach; "
        "end Worker; "
        "public class Program "
        "  public static async method Run(): Task "
        "    using var w := new Worker(); "
        "    await using var scope := new TaskScope(); "
        "    w.Attach(scope, scope); "
        "    w.Attach(o := scope, s := scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    Console.WriteLine(\"done2\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "attach\nattach\ndone\ndone2\n");
}

// A pattern binding over a TaskScope-typed scrutinee.
//
// The STDOUT is not pinned here: the `case` prints neither arm (issue #8000 —
// a PRE-EXISTING defect, measured on the base arm before this lane's change),
// so pinning the output would pin that defect. What this row pins is the
// §6.10 rule: the pattern binding is a BORROW and is admitted.
BEZEL_TEST(taskscope_owning_positions_8160,
           pattern_binding_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Check(x: Object) "
        "    case x of "
        "      var ts: TaskScope => Console.WriteLine(\"is scope\"), "
        "      _ => Console.WriteLine(\"other\") "
        "    end case; "
        "  end Check; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    Check(scope); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRan(r);
}

// A bare call statement borrowing the scope at two plain parameters.
BEZEL_TEST(taskscope_owning_positions_8160,
           bare_statement_borrow_call_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static method Log(x: Object, y: IAsyncDisposable) "
        "    Console.WriteLine(\"logged\"); "
        "  end Log; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    Log(scope, scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "logged\ndone\n");
}

// An `owned` return slot holding a legal value — the position #8118 already
// covered, still admitted for a non-TaskScope value.
BEZEL_TEST(taskscope_owning_positions_8160,
           owned_object_return_of_other_class_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Make(s: TaskScope): owned Object "
        "    Console.WriteLine(\"make\"); "
        "    return new Item(); "
        "  end Make; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    using var o := Make(scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "make\ndone\n");
}

// `scope.Token` in a child body — the self-capture's own form.
BEZEL_TEST(taskscope_owning_positions_8160,
           scope_token_in_child_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "public class Program "
        "  public static async method Child(t: CancellationToken): Task "
        "    Console.WriteLine(\"child\"); "
        "  end Child; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Child(scope.Token); "
        "    end); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "child\ndone\n");
}

// A lambda inside an `owned`-returning method returning a BORROW — the
// function-type slot that is NOT an owning one (the lambda's function type
// returns a borrow, not `owned`).
BEZEL_TEST(taskscope_owning_positions_8160,
           lambda_in_owned_method_returning_borrow_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Log(x: Object) "
        "    Console.WriteLine(\"log\"); "
        "  end Log; "
        "  public static method Make(s: TaskScope): owned Object "
        "    var pick: (TaskScope) -> TaskScope := lambda(x: TaskScope) => x; "
        "    Log(pick(s)); "
        "    Console.WriteLine(\"make\"); "
        "    return new Item(); "
        "  end Make; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    using var o := Make(scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "log\nmake\ndone\n");
}

// The block-bodied twin of the row above.
BEZEL_TEST(taskscope_owning_positions_8160,
           lambda_block_in_owned_method_returning_borrow_admitted) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Log(x: Object) "
        "    Console.WriteLine(\"log\"); "
        "  end Log; "
        "  public static method Make(s: TaskScope): owned Object "
        "    var pick: (TaskScope) -> TaskScope := lambda(x: TaskScope) => do "
        "      return x; "
        "    end; "
        "    Log(pick(s)); "
        "    Console.WriteLine(\"make\"); "
        "    return new Item(); "
        "  end Make; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    using var o := Make(scope); "
        "    await scope.Join(); "
        "    Console.WriteLine(\"done\"); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "log\nmake\ndone\n");
}

// A `case` over a NON-TaskScope scrutinee — the control that shows the
// pattern-binding position itself is admitted (the TaskScope twin above is
// the borrow §6.10 names).
BEZEL_TEST(taskscope_owning_positions_8160,
           case_control_without_taskscope_admitted_stdlib_6_10) {
    JitResult r = runAot(
        "module M; import System.Threading; " + std::string(kItem) +
        "public class Program "
        "  public static method Check(x: Object) "
        "    case x of "
        "      var it: Item => Console.WriteLine(\"is item\"), "
        "      _ => Console.WriteLine(\"other\") "
        "    end case; "
        "  end Check; "
        "  public static async method Main(): Task<int> "
        "    using var i := new Item(); "
        "    Check(i); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "is item\ndone\n");
}

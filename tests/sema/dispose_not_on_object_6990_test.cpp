// Regression tests for #6990 (§8.7, v0.340): `Dispose()` is class-only, so it
// is not a member of `Object`, and sema ACCEPTED `o.Dispose()` through an
// `Object`-typed receiver, lowering it as a header-driven dispose.
//
// §8.7 "`Dispose()` is not on `Object`", v0.340: "`Dispose()` is class-only.
// The compiler generates `IDisposable` on every non-static class. Static
// classes, records, enums, choices, primitives, and inline arrays do not
// implement `IDisposable`." §28.2 (v0.340) states the same rule for `Object`:
// "`Dispose()` is not on `Object`; it is class-only."
//
// THE DEFECT, MEASURED on e443825d (`bezelc -o out repro.bzl`): the issue's
// repro — a 100-iteration loop that moves an `Array<string>` into an
// `Object` local and calls `o.Dispose()` on it — compiled with no diagnostic,
// ran to stdout `100`, and LeakSanitizer reported 3500 bytes in 200
// allocations. The one-iteration form `var o: Object := new Array<int>();
// o.Dispose();` was accepted and exited 0.
//
// ROOT CAUSE (MEASURED by the fix: base accepts, the one arm below rejects):
// `closedValueSurfaceMemberReason` (ExprTypecheck_Calls.cpp) is the single
// closed-surface gate that both the `.` form (`resolveMemberAccess`) and the
// `?.` form (`resolveNullCondMember`) ask, and it leaves `Object` open (its
// other members are checked elsewhere). `Dispose` is also on the universal-member
// exemption lists, so nothing rejected `Dispose` on an `Object` receiver. The
// fix adds one arm to the predicate: `Dispose` on a `PrimitiveType::Object`
// receiver is a "no member" rejection that cites §8.7. The predicate runs
// before `typeCall`, so the arity block and the call machinery never see it.
//
// SCOPE. Only the `Object` receiver is changed here. A record, enum, or static
// class receiver is also accepted for `Dispose()` under §8.7; that is a
// separate defect with a different mechanism (#7022 for records and enums,
// #7023 for static classes), and it is left unchanged.
//
// Each rejection row asserts the §8.7 wording and that the program never
// reaches codegen's permissive-acceptance backstop. Each control row runs under
// the checked oracle (disposal is in play) and asserts exact stdout: the class,
// `IDisposable`, `using`, and generic-`T` receivers, the collections typed as
// themselves, `ToString` on `Object`, and the two spec-legal discharges of an
// owned `Object` value that the new diagnostic names (`_ := x;` and `using var`).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A complete module: a disposable class `D` (its `Dispose` is the §8.7 class
// disposal), then `C.Main` whose statements are `body` and which returns 0.
std::string withD(const std::string& body) {
    return "module M; "
           "class D "
           "  public constructor() end constructor; "
           "  public method Hi(): int return 7; end Hi; "
           "end D; "
           "class C "
           "  public static method Main(): int " +
           body +
           " return 0; "
           "  end Main; "
           "end C;";
}

// A rejection row: sema refuses the program at compile time, with the §8.7
// "no member" wording, and codegen's permissive backstop never runs.
void expectObjectDisposeRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "no member `Dispose` on type `Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""8.7"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "permissive acceptance"));
}

// A control row: the program compiles, runs under the checked oracle, exits 0,
// and prints exactly `expected`.
void expectRunsWithStdout(const std::string& program,
                          const std::string& expected) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ── Rejections: the Object receiver ───────────────────────────────────────

// The issue's repro (§8.7 "`Dispose()` is not on `Object`"): the loop body is
// the shape that leaked 3500 bytes in 200 allocations on the base.
BEZEL_TEST(dispose_not_on_object_6990,
           issue_repro_loop_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var i := 0; "
        "while i < 100 do "
        "  var x := new Array<string>(); "
        "  x.Add(\"a\" + \"x\"); "
        "  var o: Object := move x; "
        "  o.Dispose(); "
        "  i += 1; "
        "end; "
        "Console.WriteLine(i);"));
}

// The one-iteration form (§8.7): the minimal receiver, a boxed `Array<int>`.
BEZEL_TEST(dispose_not_on_object_6990,
           one_iteration_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var o: Object := new Array<int>(); "
        "o.Dispose(); "
        "Console.WriteLine(\"d\");"));
}

// Other collections moved into `Object` (§8.7): same receiver, different
// runtime type.
BEZEL_TEST(dispose_not_on_object_6990,
           queue_and_stack_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var q := new Queue<string>(); q.Enqueue(\"a\"); "
        "var o: Object := move q; o.Dispose(); "
        "Console.WriteLine(\"q\");"));
    expectObjectDisposeRejected(withD(
        "var s := new Stack<string>(); s.Push(\"a\"); "
        "var o: Object := move s; o.Dispose(); "
        "Console.WriteLine(\"s\");"));
}

// The null-conditional spelling (`?.`, resolveNullCondMember) of the same
// receiver (§8.7 applies to `Object?` as to `Object`).
BEZEL_TEST(dispose_not_on_object_6990,
           nullable_object_nullcond_dispose_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var o: Object? := new D(); "
        "o?.Dispose(); "
        "Console.WriteLine(\"nc\");"));
}

// A method-group reference (§27.6) to `Dispose` on an `Object` receiver, not
// a call.
BEZEL_TEST(dispose_not_on_object_6990,
           method_group_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var o: Object := new D(); "
        "var f := o.Dispose; "
        "Console.WriteLine(\"mgo\");"));
}

// Wrong arity on the same receiver (§8.1 / §8.7): the rejection is the §8.7
// one, not the arity diagnostic, because the predicate runs first.
BEZEL_TEST(dispose_not_on_object_6990,
           object_dispose_with_arguments_rejected_8_7) {
    expectObjectDisposeRejected(withD(
        "var o: Object := new D(); "
        "o.Dispose(1);"));
}

// Boxed record, enum, and primitive values (§8.6): the static type is `Object`,
// so the call is rejected whatever the runtime type of the box is.
BEZEL_TEST(dispose_not_on_object_6990,
           boxed_record_enum_primitive_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(
        "module M; "
        "record R(X: int); "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new R(1); "
        "    o.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectObjectDisposeRejected(
        "module M; "
        "enum E Low, High; end E; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := E.Low; "
        "    o.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectObjectDisposeRejected(withD(
        "var o: Object := 5; "
        "o.Dispose(); "
        "Console.WriteLine(\"bp\");"));
}

// An `Object` returned from a method, disposed on the call result (§8.7), and
// an `Object` read from an `Array<Object>` element.
BEZEL_TEST(dispose_not_on_object_6990,
           returned_and_element_object_dispose_rejected_8_7) {
    expectObjectDisposeRejected(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "end D; "
        "class G "
        "  public static method Get(): Object return new D(); end Get; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    G.Get().Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectObjectDisposeRejected(withD(
        "var xs := new Array<Object>(); "
        "var d: Object := new D(); "
        "xs.Add(move d); "
        "xs.Get(0).Dispose(); "
        "Console.WriteLine(\"idx\");"));
}

// A borrowed `Object` parameter (§28.6 would otherwise say "a non-owned
// parameter is a borrow"). The §8.7 "no member" wording now wins, which is the
// accurate reason: `owned` is not admissible on an `Object` parameter either.
BEZEL_TEST(dispose_not_on_object_6990,
           borrowed_object_param_dispose_names_8_7) {
    expectObjectDisposeRejected(
        "module M; "
        "class G "
        "  public static method Use(o: Object): int "
        "    o.Dispose(); "
        "    return 5; "
        "  end Use; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    return G.Use(5); "
        "  end Main; "
        "end C;");
}

// ── Controls: receivers whose `Dispose()` stays accepted ──────────────────

// A class receiver (§8.7 "the compiler generates IDisposable on every
// non-static class"), a field-held class, and an owned class parameter.
BEZEL_TEST(dispose_not_on_object_6990, class_receiver_dispose_still_accepted_8_7) {
    expectRunsWithStdout(withD(
        "var d := new D(); "
        "Console.WriteLine(d.Hi()); "
        "d.Dispose();"), "7\n");
    expectRunsWithStdout(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  public method Hi(): int return 4; end Hi; "
        "end D; "
        "class G "
        "  public static method Use(owned d: D): int "
        "    d.Dispose(); "
        "    return 4; "
        "  end Use; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(G.Use(new D())); "
        "    return 0; "
        "  end Main; "
        "end C;", "4\n");
}

// An `IDisposable`-typed receiver (§8.7) and `using` bindings (§28.10).
BEZEL_TEST(dispose_not_on_object_6990,
           idisposable_and_using_receivers_still_accepted_8_7) {
    expectRunsWithStdout(withD(
        "var d: IDisposable := new D(); "
        "d.Dispose(); "
        "Console.WriteLine(\"iface\");"), "iface\n");
    expectRunsWithStdout(withD(
        "using var d := new D(); "
        "Console.WriteLine(\"using\"); "
        "d.Dispose();"), "using\n");
    expectRunsWithStdout(withD(
        "using var d := new D(); "
        "Console.WriteLine(\"scope\");"), "scope\n");
}

// A generic `T` bounded by `IDisposable` (§8.7 class disposal through the
// bound), with its own `Dispose` call.
BEZEL_TEST(dispose_not_on_object_6990,
           generic_t_where_idisposable_still_accepted_8_7) {
    expectRunsWithStdout(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "end D; "
        "class G "
        "  public static method Use<T>(owned t: T): int where T: IDisposable "
        "    t.Dispose(); "
        "    return 1; "
        "  end Use; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(G.Use(new D())); "
        "    return 0; "
        "  end Main; "
        "end C;", "1\n");
}

// The collections typed as themselves (§8.7 applies to the class, and each of
// these is a class with its own disposal).
BEZEL_TEST(dispose_not_on_object_6990,
           collections_typed_as_themselves_still_accepted_8_7) {
    expectRunsWithStdout(withD(
        "var a := new Array<string>(); a.Add(\"x\"); "
        "Console.WriteLine(\"arr\"); a.Dispose();"), "arr\n");
    expectRunsWithStdout(withD(
        "var oa := new OwningArray<D>(); oa.Add(new D()); "
        "Console.WriteLine(\"oarr\"); oa.Dispose();"), "oarr\n");
    expectRunsWithStdout(withD(
        "var dd := new Dictionary<string, int>(); dd.Add(\"k\", 1); "
        "Console.WriteLine(\"dict\"); dd.Dispose();"), "dict\n");
    expectRunsWithStdout(withD(
        "var od := new OwningDictionary<string, D>(); od.Add(\"k\", new D()); "
        "Console.WriteLine(\"odict\"); od.Dispose();"), "odict\n");
}

// The discharge §8.7's rejection names for an owned `Object` value: `_ := x;`,
// an explicit disposal of a named owned binding (§17.13; §28.3 K3.9).
BEZEL_TEST(dispose_not_on_object_6990,
           discard_remedy_discharges_owned_object_17_13) {
    expectRunsWithStdout(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new Array<int>(); "
        "    _ := o; "
        "    Console.WriteLine(\"disc\"); "
        "    return 0; "
        "  end Main; "
        "end C;", "disc\n");
}

// The other discharge §8.7's rejection names: `using var` on the owned value
// (§28.10), disposed at scope end.
BEZEL_TEST(dispose_not_on_object_6990,
           using_var_remedy_discharges_owned_object_28_10) {
    expectRunsWithStdout(withD(
        "var x := new Array<string>(); "
        "using var o: Object := move x; "
        "Console.WriteLine(\"using\");"), "using\n");
}

// `ToString` on an `Object` receiver is a §8.1 universal member and stays
// accepted: the rejection is for `Dispose` alone.
BEZEL_TEST(dispose_not_on_object_6990, object_tostring_still_accepted_8_1) {
    expectRunsWithStdout(withD(
        "var o: Object := new D(); "
        "Console.WriteLine(o.ToString().Length > 0);"), "True\n");
}

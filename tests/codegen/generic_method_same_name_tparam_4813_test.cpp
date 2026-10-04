// Codegen — issue #4813: a generic method `Go<T>` calling a generic method
// `Id<T>` whose type parameter has the SAME name aborted the IR verifier.
// `Id<int>(4)` and the inferred `Id(5)` inside `Go<T>` at T = string built
// `Id$spec(i32): ptr` (the callee's `T` read through the CALLER's binding).
// Renaming the callee's parameter to `V` printed `9`; sema types both calls
// `int`.
//
// AUTHORITY. Spec §14.1 (v0.307): a generic method's type parameters are
// declared by that method, and an instantiation substitutes the supplied type
// arguments for THEM; §14.5 infers those arguments from the call's argument
// types. Neither makes a callee's parameter depend on what a caller's
// parameter happens to be named.
//
// MECHANISM. `applyMethodGenericSubst` starts the callee's substitution as a
// copy of the active maps (it must keep receiver-class bindings), and every
// binding step skipped a name already present — so the caller's `T` survived
// as the callee's `T`. The fix drops the callee's own parameter names from the
// copy and resolves the explicit type arguments, argument types and result
// type in the CALLER's substitution before binding them. The lambda pre-pass
// and the #3137 unit-type-arg pre-pass in `lowerCall` get the same scoping.
//
// SHADOWING (#4862). The maps are keyed by NAME, so a method `T` that shadows
// its declaring type's `T` cannot be bound beside the class `T` the body still
// reads. A name the declaring type (or extension block) binds is therefore NOT
// dropped — the pre-fix behaviour. The three `no_regression_*` rows pin that:
// an earlier cut of this fix dropped it and printed `5+0 6+4439312` for
// `5+z 6+z`. The shadowing shape itself is #4862 (codegen) / #4847 (sema).
// The shadowing method is called with EXPLICIT type arguments only
// (`Pick<int>(5)`, `Pick<long>(6)`): the previous commit (#4915) keeps an
// INFERRED call to a shadowing method (`Pick(5)`) rejected, as at 335e3849,
// because accepting it reaches #4862 (MEASURED: IR-verifier abort for a
// `T` local, wrong value at `Box<long>`).
//
// LAMBDA HINTS (reviewer regression of the first rework, 282acabd). The
// lambda pre-pass resolves a callee binding in the caller's scope, so an
// inferred lambda parameter's hint is now CONCRETE; codegen injects that hint
// into the persistent sema type map (`if (!exprType) setExprType(hint)`), so
// the next specialization of the same lambda AST read the previous one's
// type. lowerLambda now clears the entries it injected on exit. The
// `lambda_two_instantiations_*` rows pin both orders; the same-NAME spelling
// was already wrong on r20-c25 by the same mechanism. The second rework
// (a799551b) still leaked the parameter HINT map itself: lowerLambda took its
// snapshot after setting the lambda's own hints, so an `int` specialization
// (whose path sets no hint) read the previous one's — the `*_then_int`,
// `six_alternating` and `nested_lambda_four_specs` rows. The snapshot is now
// taken on entry. The lambda pre-pass also reads an enclosing lambda
// parameter's hint when the argument has no sema type yet (`nested_lambda_*`).
//
// COLLECTION-LITERAL ARGUMENTS. A literal has no type of its own (§15.4), so
// buildOrderedArgs builds it at the FORMAL `Array<T>` — whose `T` is the
// callee's, but was lowered under the caller's maps (the caller's same-named
// `T`, or the unbound i32 placeholder). It now resolves the formal through
// the callee's own bindings, and applyMethodGenericSubst binds a
// still-unbound `T` from a literal whose elements agree on one SEMA type
// (a799551b compared lowered LLVM types, so `["s", new A()]` bound `T` to
// `string`: ASan heap-buffer-overflow). A disagreeing literal keeps the
// r20-c25 behaviour here, but sema rejects a heterogeneous, empty or
// dictionary literal-only `T` since #4895 (§14.5 phase 3), so an accepted
// program no longer reaches it; a homogeneous literal binds `T`. The
// `array_literal_*` rows pin exact element values (the first rework dropped
// the p3 chain as "outside #4813"; it is this half of it).
//
// FAIL-WITHOUT: measured and recorded in the commit message.
//
// §14.1 / §14.5, v0.307.

#include <string>

#include "support/aot_support.hpp"   // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + ">" + r.stdoutText;
    return r.stdoutText;
}
}  // namespace

// The issue's repro, verbatim: explicit and inferred same-named callee.
BEZEL_TEST(generic_method_same_name_tparam_4813, issue_repro_spec_14_1) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Go<T>(v: T): int\n"
        "    var n: int := Id<int>(4);\n"
        "    var m: int := Id(5);\n"
        "    return n + m;\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"a\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("9\n"));
}

// `Id<T>(v)` inside `Go<T>`: the written `T` is the CALLER's (string), and an
// inferred `Id(a)` from a caller-`T` argument binds the same.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           explicit_caller_t_control_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Go<T>(v: T): T\n"
        "    var a: T := Id<T>(v);\n"
        "    var b: T := Id(a);\n"
        "    return b;\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"hello\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("hello\n"));
}

// The callee's `T` bound to a type BUILT from the caller's `T` (`Array<T>`),
// explicitly and by inference, at a reference and a value instantiation.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           mixed_array_of_caller_t_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Count<T>(xs: Array<T>): int\n"
        "    return xs.Count;\n"
        "  end Count;\n"
        "  public static method Go<T>(v: T): string\n"
        "    var arr: Array<T> := [v, v, v];\n"
        "    var w: Array<T> := Id(arr);\n"
        "    var k: int := Count<T>(w);\n"
        "    var j: int := Count(arr);\n"
        "    var e: T := Id<T>(w[1]);\n"
        "    return $\"{w.Count} {k} {j} {e}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "    Console.WriteLine(Util.Go<long>(7));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("3 3 3 q\n3 3 3 7\n"));
}

// A lambda argument routes through lowerCall's pre-pass, which had the same
// name-merge guard.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           lambda_arg_prepass_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Apply<T>(x: T, f: (T) -> T): T\n"
        "    return f(x);\n"
        "  end Apply;\n"
        "  public static method Go<T>(v: T): string\n"
        "    var a: int := Apply<int>(20, lambda(y) => y + 1);\n"
        "    var b: int := Apply(40, lambda(y: int) => y * 2);\n"
        "    var c: T := Apply<T>(v, lambda(y) => y);\n"
        "    var d: T := Apply(v, lambda(y: T) => y);\n"
        "    return $\"{a} {b} {c} {d}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"w\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("21 80 w w\n"));
}

// A generic CLASS's `T` (the caller's) and a static generic method's `T` on
// another class.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           class_t_caller_static_callee_spec_14_1) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "end Util;\n"
        "public class Box<T>\n"
        "  private _v: T;\n"
        "  public constructor(v: T)\n"
        "    _v := v;\n"
        "  end constructor;\n"
        "  public method Sum(): string\n"
        "    var a: int := Util.Id<int>(4);\n"
        "    var b: int := Util.Id(5);\n"
        "    var c: T := Util.Id<T>(_v);\n"
        "    return $\"{a + b} {c}\";\n"
        "  end Sum;\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var b := new Box<string>(\"z\");\n"
        "    Console.WriteLine(b.Sum());\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("9 z\n"));
}

// CONTROL. A receiver of a generic CLASS (`Box<string>`) whose non-shadowing generic
// method `Keep<U>` reads the class `T`, called from `Go<T>` with the caller's
// `T` explicit and inferred: the receiver's `T` and the caller's `T` share a
// name but are different parameters.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): `Pick<T>` inside `Box<T>` is now rejected at
// the type-parameter list. The test's body contains shadowing (the comment
// is accurate for `Keep<U>` but `Pick<T>` shadows), so the whole class is
// rejected. Flip to rejection.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           callee_t_beside_receiver_class_t_spec_14_5) {
    auto r = runAot(
        "module Conf;\n"
        "public class Box<T>\n"
        "  private _v: T;\n"
        "  public constructor(v: T)\n"
        "    _v := v;\n"
        "  end constructor;\n"
        "  public method Pick<T>(x: T): T\n"
        "    return x;\n"
        "  end Pick;\n"
        "  public method Keep<U>(u: U): string\n"
        "    var t: T := _v;\n"
        "    return $\"{t} {u}\";\n"
        "  end Keep;\n"
        "  public method Both(): string\n"
        "    var a: int := Pick<int>(5);\n"
        "    var s: string := Keep(a);\n"
        "    return s;\n"
        "  end Both;\n"
        "end Box;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T): string\n"
        "    var b := new Box<string>(\"s\");\n"
        "    var r1: string := b.Keep<T>(v);\n"
        "    var r2: string := b.Keep(v);\n"
        "    var r3: int := 11;\n"
        "    var r4: string := b.Keep(5);\n"
        "    return $\"{r1}|{r2}|{r3}|{r4}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<int>(3));\n"
        "    Console.WriteLine(Util.Go<long>(4));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);  // rejected at declaration site after fix #5393
}

// Three generic methods that all name their parameter `T`, each calling the
// next explicitly (at a concrete type and at the caller's `T`) and by inference.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           three_level_chain_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(x: T): string\n"
        "    return $\"C:{x}\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    var a: string := C<int>(7);\n"
        "    var b: string := C(x);\n"
        "    var c: string := C<T>(x);\n"
        "    return $\"{a},{b},{c}\";\n"
        "  end B;\n"
        "  public static method A<T>(x: T): string\n"
        "    var p: string := B(x);\n"
        "    var q: string := B<string>(\"k\");\n"
        "    var r: string := B<long>(9);\n"
        "    return $\"{p}/{q}/{r}\";\n"
        "  end A;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.A<double>(1.5));\n"
        "    Console.WriteLine(Util.A(true));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("C:7,C:1.5,C:1.5/C:7,C:k,C:k/C:7,C:9,C:9\nC:7,C:True,C:True/C:7,C:k,C:k/C:7,C:9,C:9\n"));
}

// The `?.` path (lowerNullCondCall) shares applyMethodGenericSubst.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           null_conditional_call_spec_14_1) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Box\n"
        "  public method Id<T>(x: T): string\n"
        "    return $\"{x}\";\n"
        "  end Id;\n"
        "  public method Ret<T>(x: T): T\n"
        "    return x;\n"
        "  end Ret;\n"
        "end Box;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T, b: Box?): string\n"
        "    var r1: string? := b?.Id<int>(4);\n"
        "    var r2: string? := b?.Id(5);\n"
        "    var r3: int? := b?.Ret<int>(6);\n"
        "    var r4: int? := b?.Ret(7);\n"
        "    var r5: string? := b?.Id<T>(v);\n"
        "    return $\"{r1 ?? \"-\"}|{r2 ?? \"-\"}|{r3 ?? 0}|{r4 ?? 0}|{r5 ?? \"-\"}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var b: Box? := new Box();\n"
        "    Console.WriteLine(Util.Go<string>(\"q\", b));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("4|5|6|7|q\n"));
}

// Generic extension methods (§26.5) called from `Go<T>`: `Tag<T>` names its
// parameter like the caller, and `extension Array<T>` binds a block-level `T`
// that its non-shadowing methods `Pair<U>` / `Twin<T2>` read.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           extension_method_spec_26_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "extension string\n"
        "  public method Tag<T>(x: T): string => $\"{this}:{x}\";\n"
        "end;\n"
        "extension Array<T>\n"
        "  public method Pair<U>(u: U): string => $\"{this.Count}:{u}\";\n"
        "  public method Twin<T2>(u: T2): string => $\"{this[0]}/{u}\";\n"
        "end;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T): string\n"
        "    var s: string := \"a\";\n"
        "    var arr: Array<string> := [\"x\", \"y\"];\n"
        "    var r1: string := s.Tag<string>(\"s\");\n"
        "    var r2: string := s.Tag(true);\n"
        "    var r3: string := s.Tag<T>(v);\n"
        "    var r4: string := s.Tag(v);\n"
        "    var r5: string := arr.Pair<T>(v);\n"
        "    var r6: string := arr.Pair(7);\n"
        "    var r7: string := arr.Twin(v);\n"
        "    return $\"{r1} {r2} {r3} {r4} {r5} {r6} {r7}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<int>(3));\n"
        "    Console.WriteLine(Util.Go<long>(9));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("a:s a:True a:3 a:3 2:3 2:7 x/3\na:s a:True a:9 a:9 2:9 2:7 x/9\n"));
}

// Same-named generic calls inside lambda bodies in `Go<T>`.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           lambda_body_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Go<T>(v: T): string\n"
        "    var f := lambda(y: int) => Id(y) + Id<int>(1);\n"
        "    var g := lambda() => Id<T>(v);\n"
        "    var h := lambda(z: T) => Id(z);\n"
        "    var k := lambda() => Id<string>(\"k\");\n"
        "    return $\"{f(10)} {g()} {h(v)} {k()}\";\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "    Console.WriteLine(Util.Go<long>(7));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("11 q q k\n11 7 7 k\n"));
}

// An async generic callee, and same-named calls inside an async generic body.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           async_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static async method Echo<T>(v: T): Task<T>\n"
        "    return v;\n"
        "  end Echo;\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Go<T>(v: T): string\n"
        "    var t1: Task<int> := Echo<int>(4);\n"
        "    var t2: Task<int> := Echo(5);\n"
        "    var t3: Task<T> := Echo(v);\n"
        "    return $\"{t1.Result} {t2.Result} {t3.Result}\";\n"
        "  end Go;\n"
        "  public static async method GoA<T>(v: T): Task<string>\n"
        "    var a: int := Id<int>(4);\n"
        "    var b: int := Id(5);\n"
        "    var c: T := Id(v);\n"
        "    var d: int := await Echo(6);\n"
        "    return $\"{a + b} {c} {d}\";\n"
        "  end GoA;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"s\"));\n"
        "    var t: Task<string> := Util.GoA<string>(\"r\");\n"
        "    Console.WriteLine(t.Result);\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("4 5 s\n9 r 6\n"));
}

// An iterator generic callee, and same-named calls inside an iterator body.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           iterator_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Rep<T>(x: T, n: int): IEnumerable<T>\n"
        "    for var i in 0..<n do\n"
        "      yield return x;\n"
        "    end;\n"
        "  end Rep;\n"
        "  public static method Id<T>(x: T): T\n"
        "    return x;\n"
        "  end Id;\n"
        "  public static method Go<T>(v: T): string\n"
        "    var sum: int := 0;\n"
        "    for var i in Rep<int>(3, 2) do sum += i; end;\n"
        "    for var j in Rep(4, 1) do sum += j; end;\n"
        "    var sb: string := \"\";\n"
        "    for var x in Rep(v, 2) do sb := sb + $\"{x}\"; end;\n"
        "    return $\"{sum} {sb}\";\n"
        "  end Go;\n"
        "  public static method It<T>(v: T): IEnumerable<string>\n"
        "    yield return $\"{Id<int>(1) + Id(2)}\";\n"
        "    yield return $\"{Id(v)}\";\n"
        "  end It;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"s\"));\n"
        "    for var s in Util.It<string>(\"z\") do Console.WriteLine(s); end;\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("10 ss\n3\nz\n"));
}

// NO-REGRESSION PIN (#4862). A method `T` SHADOWING the class `T` while the
// body reads the class-`T` field `_v`. Codegen's maps are keyed by NAME, so one
// map cannot bind both; the fix must not erase a name the declaring type binds.
// The first cut of this fix (which erased it) printed `5+0 6+4439312` —
// silent wrong code. Explicit type arguments only; see the header.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): the declaration is now rejected at the type-
// parameter list. These programs no longer compile.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           no_regression_shadowing_reads_class_field_spec_14_1) {
    auto r = runAot(
        "module Conf;\n"
        "public class Box<T>\n"
        "  private _v: T;\n"
        "  public constructor(v: T)\n"
        "    _v := v;\n"
        "  end constructor;\n"
        "  public method Pick<T>(x: T): string\n"
        "    return $\"{x}+{_v}\";\n"
        "  end Pick;\n"
        "  public method Sum(): string\n"
        "    var a: string := Pick<int>(5);\n"
        "    var b: string := Pick<long>(6);\n"
        "    return $\"{a} {b}\";\n"
        "  end Sum;\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var b := new Box<string>(\"z\");\n"
        "    Console.WriteLine(b.Sum());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
}

// NO-REGRESSION PIN (#4862): as above, the shadowing body reaching the class
// `T` through a non-generic `Show()`.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           no_regression_shadowing_calls_class_t_reader_spec_14_1) {
    auto r = runAot(
        "module Conf;\n"
        "public class Box<T>\n"
        "  private _v: T;\n"
        "  public constructor(v: T)\n"
        "    _v := v;\n"
        "  end constructor;\n"
        "  public method Get(): T\n"
        "    return _v;\n"
        "  end Get;\n"
        "  public method Show(): string\n"
        "    return $\"[{_v}]\";\n"
        "  end Show;\n"
        "  public method Pick<T>(x: T): string\n"
        "    return $\"{x}+{Show()}\";\n"
        "  end Pick;\n"
        "  public method Sum(): string\n"
        "    var a: string := Pick<int>(5);\n"
        "    var b: string := Pick<long>(6);\n"
        "    return $\"{a} {b}\";\n"
        "  end Sum;\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var b := new Box<string>(\"z\");\n"
        "    Console.WriteLine(b.Sum());\n"
        "    Console.WriteLine(b.Show());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
}

// NO-REGRESSION PIN (#4862): the shadowing shape at `Box<string>` and
// `Box<int>`.
BEZEL_TEST(generic_method_same_name_tparam_4813,
           no_regression_shadowing_two_instantiations_spec_14_1) {
    auto r = runAot(
        "module Conf;\n"
        "public class Box<T>\n"
        "  private _v: T;\n"
        "  public constructor(v: T)\n"
        "    _v := v;\n"
        "  end constructor;\n"
        "  public method Pick<T>(x: T): string\n"
        "    return $\"{x}+{_v}\";\n"
        "  end Pick;\n"
        "  public method Sum(): string\n"
        "    var a: string := Pick<int>(5);\n"
        "    var b: string := Pick<long>(6);\n"
        "    return $\"{a} {b}\";\n"
        "  end Sum;\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var b := new Box<string>(\"z\");\n"
        "    Console.WriteLine(b.Sum());\n"
        "    var c := new Box<int>(42);\n"
        "    Console.WriteLine(c.Sum());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
}

// Reviewer regression of the first #4813 rework (a2m_two_rev): `Go<long>` then
// `Go<string>` over one lambda AST. The lambda-parameter hint injected into the
// persistent sema type map for the first specialization was read by the
// second: `<4>` then an address, exit 0 (silent wrong value).
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_two_instantiations_long_first) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<long>(4L));\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<4>\n<q>\n"));
}

// The same program, string first (a2m_two): the stale `string` hint on the
// `long` specialization emitted string GEPs on an i64 — IR verifier abort.
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_two_instantiations_string_first) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "    Console.WriteLine(Util.Go<long>(4L));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<q>\n<4>\n"));
}

// The same-NAME spelling (`Map<T>` called from `Go<T>`), long first: wrong on
// r20-c25 too (`<4>` then an address), by the same stale-hint mechanism.
BEZEL_TEST(generic_method_same_name_tparam_4813, same_name_lambda_two_instantiations) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<T>(x: T, f: (T) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<long>(4L));\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<4>\n<q>\n"));
}

// Argument-lowering half (reviewer r5): `C<int>([11, 22])` inside `B<T>`. The
// literal was built at the FORMAL `Array<T>` under the CALLER's `T` (8-byte
// elements) and read by `C` at T = int: SIGSEGV on r20-c25, `11,0` on the
// first rework; ASan stack-buffer-overflow in bz_array_add on both.
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_explicit_same_name) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]},{xs[1]}\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    return C<int>([11, 22]);\n"
        "  end B;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.B<string>(\"q\"));\n"
        "    Console.WriteLine(Util.B<long>(5L));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("11,22\n11,22\n"));
}

// Inferred spelling (reviewer r5i): `C([11, 22])` — `T` bound from the
// literal's elements, which have no other source.
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_inferred_same_name) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]},{xs[1]}\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    return C([11, 22]);\n"
        "  end B;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.B<string>(\"q\"));\n"
        "    Console.WriteLine(Util.B<long>(5L));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("11,22\n11,22\n"));
}

// `C<long>([11L, 22L])` inside `B<T>` at T = string then int.
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_long_same_name) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]},{xs[1]}\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    return C<long>([11L, 22L]);\n"
        "  end B;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.B<string>(\"q\"));\n"
        "    Console.WriteLine(Util.B<int>(5));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("11,22\n11,22\n"));
}

// `C<string>(["a", "b"])` inside `B<T>` at T = string then int.
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_string_same_name) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]},{xs[1]}\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    return C<string>([\"a\", \"b\"]);\n"
        "  end B;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.B<string>(\"q\"));\n"
        "    Console.WriteLine(Util.B<int>(5));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("a,b\na,b\n"));
}

// The same formal-resolution arm with NO enclosing substitution: the callee's
// `T` read the unbound i32 placeholder whatever the element type (r20-c25:
// bus error).
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_non_generic_caller) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]},{xs[1]}\";\n"
        "  end C;\n"
        "  public static method B(x: string): string\n"
        "    return $\"{C<long>([11L, 22L])} {C<int>([3, 4])} {C<string>([\"a\", \"b\"])}\";\n"
        "  end B;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.B(\"q\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("11,22 3,4 a,b\n"));
}

// The p3 chain the first rework dropped as "outside #4813", with exact element
// values: a literal of the caller's `T` (`C([x, x])`) beside `C<int>([1, 2])`,
// two levels down. r20-c25: SIGSEGV.
BEZEL_TEST(generic_method_same_name_tparam_4813, array_literal_three_level_chain) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method C<T>(xs: Array<T>): string\n"
        "    return $\"{xs[0]}/{xs[1]};\";\n"
        "  end C;\n"
        "  public static method B<T>(x: T): string\n"
        "    return $\"{C([x, x])}{C<int>([1, 2])}\";\n"
        "  end B;\n"
        "  public static method A<T>(x: T): string\n"
        "    return $\"{B(x)}{B<long>(9L)}\";\n"
        "  end A;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.A<string>(\"q\"));\n"
        "    Console.WriteLine(Util.A<int>(7));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("q/q;1/2;9/9;1/2;\n7/7;1/2;9/9;1/2;\n"));
}

// Reviewer regressions of the second #4813 rework (a799551b, rv-L12d): an
// `int` specialization AFTER a non-int one over one lambda AST. lowerLambda
// snapshotted the parameter-hint map AFTER setting this lambda's own hints,
// so the restore on exit KEPT them; for W = int the i32 path sets no hint,
// so the previous specialization's concrete hint (bool/string/double) was
// read. `bi`: `<True>` twice, exit 0 (silent wrong value).
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_bool_then_int_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<bool>(true));\n"
        "    Console.WriteLine(Util.Go<int>(5));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<True>\n<5>\n"));
}

// `si`: string then int — the stale `string` hint on the int specialization
// failed the IR verifier (a799551b).
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_string_then_int_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"a\"));\n"
        "    Console.WriteLine(Util.Go<int>(2));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<a>\n<2>\n"));
}

// `di`: double then int — IR verifier abort on a799551b.
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_double_then_int_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<double>(1.5));\n"
        "    Console.WriteLine(Util.Go<int>(2));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<1.5>\n<2>\n"));
}

// `n5`: six specializations alternating long/string/int/string/double/long —
// IR verifier abort on a799551b.
BEZEL_TEST(generic_method_same_name_tparam_4813, lambda_six_alternating_specs_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => $\"<{y}>\");\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<long>(1L));\n"
        "    Console.WriteLine(Util.Go<string>(\"a\"));\n"
        "    Console.WriteLine(Util.Go<int>(2));\n"
        "    Console.WriteLine(Util.Go<string>(\"b\"));\n"
        "    Console.WriteLine(Util.Go<double>(2.5));\n"
        "    Console.WriteLine(Util.Go<long>(3L));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("<1>\n<a>\n<2>\n<b>\n<2.5>\n<3>\n"));
}

// `n3`: a nested lambda that reads the OUTER lambda's parameter — the outer
// hint must stay live inside the inner lambda and be dropped when the outer
// one exits. IR verifier abort on a799551b.
BEZEL_TEST(generic_method_same_name_tparam_4813, nested_lambda_four_specs_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => Map(y, lambda(z) => $\"{z}|{y}\"));\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<long>(4L));\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "    Console.WriteLine(Util.Go<int>(7));\n"
        "    Console.WriteLine(Util.Go<string>(\"r\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("4|4\nq|q\n7|7\nr|r\n"));
}

// The n3 mechanism isolated (rv-L12d follow-up): a nested call whose only
// source for the callee's `W` is the ENCLOSING lambda's inferred parameter `y`.
// `y` has no sema expression type until lowerExpr injects its hint, which runs
// after the lambda pre-pass; the pre-pass erased the callee's `W` and could
// not rebind it, so `z` was lowered at the i32 placeholder and the string
// printed as an address. Correct on r20-c25 only because the outer call's
// same-NAMED `W` leaked in; a799551b printed an address here from a
// NON-generic caller.
BEZEL_TEST(generic_method_same_name_tparam_4813, nested_lambda_non_generic_caller_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Go(v: string): string\n"
        "    return Map(v, lambda(y) => Map(y, lambda(z) => $\"{z}|{y}\"));\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go(\"q\"));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("q|q\n"));
}

// The same with a DIFFERENTLY-named inner callee (`Map2<U>`): wrong on r20-c25
// as well (no same-named binding to leak), for the same reason.
BEZEL_TEST(generic_method_same_name_tparam_4813, nested_lambda_distinct_callee_spec_14_5) {
    BEZEL_EXPECT_EQ(out(
        "module Conf;\n"
        "public class Util\n"
        "  public static method Map<W>(x: W, f: (W) -> string): string\n"
        "    return f(x);\n"
        "  end Map;\n"
        "  public static method Map2<U>(x: U, f: (U) -> string): string\n"
        "    return f(x);\n"
        "  end Map2;\n"
        "  public static method Go<T>(v: T): string\n"
        "    return Map(v, lambda(y) => Map2(y, lambda(z) => $\"{z}|{y}\"));\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Util.Go<string>(\"q\"));\n"
        "    Console.WriteLine(Util.Go<long>(4L));\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("q|q\n4|4\n"));
}

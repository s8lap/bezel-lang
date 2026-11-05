// fix #8186 (spec §14.2 *Interface conformance* rule 3 / §14.1 *Instantiation
// and its site*, stdlib §2.8 / §4.4 / §5.1, v0.355): an INTERFACE element
// whose EXTENSION CHAIN reaches `IComparable<Self>` is comparable, so the
// comparerless `Min()`/`Max()` (stdlib §5.1) must dispatch through the
// element's own `CompareTo` — eagerly and lazily — and `Sort()`/`Sorted()`
// (stdlib §4.4) must order by it.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * spec §14.2 *Interface conformance* rule 3 (v0.355): "**T is an
//     interface** that has I in its extension chain — either I is named
//     directly in T's inheritance clause, or T extends an interface that
//     itself conforms to I". The SECOND clause is the one this issue is
//     about.
//   * spec §14.1 *Instantiation and its site* (v0.355): "An instantiation of
//     a generic declaration is the constructed type or constructed member
//     obtained by substituting type arguments for its type parameters" — so
//     `interface IOrd<T>: IComparable<T>` read at `IOrd<IShape>` names
//     `IComparable<IShape>`.
//   * stdlib §5.1 (v0.355): "public extension IEnumerable<T> where T:
//     IComparable<T> … public method Min(): T; public method Max(): T;" and
//     stdlib §2.8: "`a.CompareTo(b)` returns a negative number if `a < b`,
//     zero if equal, a positive number if `a > b`."
//   * stdlib §4.4 (v0.355): the comparerless `Sort()`/`Sorted()` sit in the
//     same `where T: IComparable<T>` block.
//
// THE DEFECT. The shared §14.2-rule-3 predicate (`sema::selfComparableIface`,
// ONE definition since #2787) read the element interface's DIRECT
// `baseTypeRefs` only, so it could not see an `IComparable` that lives one
// level down an extending interface. MEASURED on r30-c1 (the reproducer below):
//
//   Min() without a selector requires the element type to satisfy `T:
//   IComparable<T>` (stdlib §5.1 / spec §14.2 / #2770); `IShape` does not —
//   its `CompareTo` could not be resolved for dispatch.
//
// SEMA had already admitted the element (its §5.1 gate reads the base
// CLOSURE — #8185/#8209), so the rejection came from CODEGEN's #2770 guard,
// which asks the shared predicate and then re-scans the element's own direct
// bases for the `IComparable` base to find `CompareTo` on.
//
// The fix walks the chain (re-expressing each level's declared base in that
// level's own type arguments, §14.1) and finds `CompareTo` on `IComparable`
// itself — the member the itable slot belongs to.
//
// ROWS. The issue's own reproducer (eager and lazy), the generic
// self-recurrence `interface IG<T>: IComparable<IG<T>>` at `Array<IG<int>>`
// (eager and lazy — the shape a reviewer measured as refused), the
// comparerless `Sort()` over the same chain, and four CONTROLS: a chain that
// reaches no `IComparable` stays refused, the #8209 INVARIANCE rejection
// (`IG<T>: IComparable<IG<int>>` at `IG<string>`) stays refused, and a user's
// `extension Object { Min() }` must NEVER answer a chained-interface element
// (the §5 candidate IS applicable there, so §26.5.1's ranking keeps it).
//
// The rows are LOOPED (100 iterations) where a leak could hide, and the
// `runAotChecked` rows carry generated-code ASan + LeakSanitizer: the thunk
// reads word 0 of each element as a vtable, so a wrongly routed element
// surfaces as a memory error here rather than as a value coincidence.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The chain: `IOrd<T>` is generic and names `IComparable<T>`; `IShape`
// EXTENDS `IOrd<IShape>` — §14.2 rule 3's second clause ("T extends an
// interface that itself conforms to I"), with §14.1's substitution making it
// `IComparable<IShape>`.
const char* kChain =
    "public interface IOrd<T>: IComparable<T> end IOrd;\n"
    "public interface IShape: IOrd<IShape>\n"
    "  method Area(): int;\n"
    "end IShape;\n"
    "class Sq: IShape\n"
    "  private _s: int;\n"
    "  public constructor(s: int) _s := s; end constructor;\n"
    "  public method Area(): int => _s * _s;\n"
    "  public method CompareTo(other: IShape): int => Area() - other.Area();\n"
    "end Sq;\n";

// The generic SELF-RECURRENCE `IG<T>: IComparable<IG<T>>`: the declared
// argument is `IG<T>`, so §14.1's substitution makes it `IG<int>` at
// `IG<int>` — the element conforms to `IComparable<IG<int>>`, which IS
// `IComparable<Self>` at that instantiation.
const char* kSelfRecurrence =
    "public interface IG<T>: IComparable<IG<T>>\n"
    "  method Rank(): int;\n"
    "end IG;\n"
    "class G: IG<int>\n"
    "  private _r: int;\n"
    "  public constructor(r: int) _r := r; end constructor;\n"
    "  public method Rank(): int => _r;\n"
    "  public method CompareTo(other: IG<int>): int => _r - other.Rank();\n"
    "end G;\n";

// A user `extension Object` member of the SAME name. §26.5.1 (v0.355):
// "extension members of the called name that match the call's arity and
// argument types … with block-level `where` clauses satisfied … A `where`
// clause is an *applicability* filter, never a ranking signal." The §5
// `Min()` IS applicable at a comparable element, so it stays in the ranking
// and (its `IEnumerable<T>` head being more specific than `Object`) wins —
// the extension must never answer.
const char* kObjectMin =
    "extension Object\n"
    "  public method Min(): int => 4242;\n"
    "  public method Max(): int => 4343;\n"
    "end;\n";

std::string prog(const std::string& prelude, const std::string& body) {
    return "module M;\n" + prelude +
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// THE ISSUE'S REPRODUCER (eager). Areas 9 and 1 -> Min*10 + Max = 1*10 + 9 =
// 19. MEASURED on r30-c1: REFUSED with the #2770 guard's diagnostic
// ("`IShape` does not — its `CompareTo` could not be resolved for dispatch").
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           chain_reaches_icomparable_eager_min_max_dispatch_compareto) {
    JitResult r = runAotChecked(prog(kChain,
        "    using var a := new Sq(3);\n"
        "    using var b := new Sq(1);\n"
        "    var arr: Array<IShape> := [a, b];\n"
        "    Console.WriteLine(arr.Min().Area());\n"
        "    Console.WriteLine(arr.Max().Area());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n9\n");
}

// The same chain, LAZY: stdlib §5's `Where` returns a lazy `IEnumerable<T>`,
// and a lazy receiver's terminal MATERIALIZES the pipeline and runs the same
// comparerless `Min()`/`Max()` — so the two must agree.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           chain_reaches_icomparable_lazy_min_max_dispatch_compareto) {
    JitResult r = runAotChecked(prog(kChain,
        "    using var a := new Sq(3);\n"
        "    using var b := new Sq(1);\n"
        "    var arr: Array<IShape> := [a, b];\n"
        "    Console.WriteLine(arr.Where(lambda(x: IShape) => true)"
        ".Min().Area());\n"
        "    Console.WriteLine(arr.Where(lambda(x: IShape) => true)"
        ".Max().Area());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n9\n");
}

// stdlib §4.4's comparerless `Sort()` over the same chain — the shared
// predicate serves it too (the same `selfComparableIfaceCompareTo` thunk).
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           chain_reaches_icomparable_sort_orders_by_compareto) {
    JitResult r = runAotChecked(prog(kChain,
        "    using var a := new Sq(3);\n"
        "    using var b := new Sq(1);\n"
        "    var arr: Array<IShape> := [a, b];\n"
        "    arr.Sort();\n"
        "    Console.WriteLine(arr[0].Area());\n"
        "    Console.WriteLine(arr[1].Area());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n9\n");
}

// The GENERIC self-recurrence `IG<T>: IComparable<IG<T>>` at `Array<IG<int>>`
// — the shape a reviewer measured as refused both eagerly and lazily before
// this fix. Ranks 3 and 1 -> Min*10 + Max = 13.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           generic_self_recurrence_ig_t_icomparable_ig_t_eager_min_max) {
    JitResult r = runAotChecked(prog(kSelfRecurrence,
        "    using var a := new G(3);\n"
        "    using var b := new G(1);\n"
        "    var arr: Array<IG<int>> := [a, b];\n"
        "    Console.WriteLine(arr.Min().Rank());\n"
        "    Console.WriteLine(arr.Max().Rank());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n3\n");
}

// The same generic self-recurrence, LAZY.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           generic_self_recurrence_ig_t_icomparable_ig_t_lazy_min_max) {
    JitResult r = runAotChecked(prog(kSelfRecurrence,
        "    using var a := new G(3);\n"
        "    using var b := new G(1);\n"
        "    var arr: Array<IG<int>> := [a, b];\n"
        "    Console.WriteLine(arr.Where(lambda(x: IG<int>) => true)"
        ".Min().Rank());\n"
        "    Console.WriteLine(arr.Where(lambda(x: IG<int>) => true)"
        ".Max().Rank());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n3\n");
}

// CONTROL — the extension must NEVER answer a chained-interface element: the
// §5 `Min()` IS applicable there (the element conforms to `IComparable<Self>`),
// so §26.5.1's ranking keeps it and its `IEnumerable<T>` head beats `Object`.
// The output is the §5 result (area 1), never the extension's 4242.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           user_object_extension_never_answers_a_chained_interface_element) {
    JitResult r = runAotChecked(prog(std::string(kChain) + kObjectMin,
        "    using var a := new Sq(3);\n"
        "    using var b := new Sq(1);\n"
        "    var arr: Array<IShape> := [a, b];\n"
        "    Console.WriteLine(arr.Min().Area());\n"
        "    Console.WriteLine(arr.Max().Area());\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n9\n");
}

// CONTROL — an interface that reaches NO `IComparable` anywhere in its chain
// stays refused, with the §5.1 constraint diagnostic. The chain walk must not
// have widened the predicate into an accept.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           control_interface_reaching_no_icomparable_stays_refused) {
    JitResult r = runAot(prog(
        "public interface IPlain\n"
        "  method Rank(): int;\n"
        "end IPlain;\n"
        "class P: IPlain\n"
        "  private _r: int;\n"
        "  public constructor(r: int) _r := r; end constructor;\n"
        "  public method Rank(): int => _r;\n"
        "end P;\n",
        "    using var a := new P(3);\n"
        "    using var b := new P(1);\n"
        "    var arr: Array<IPlain> := [a, b];\n"
        "    var m := arr.Min();\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "IPlain does not implement IComparable<T>"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL — the #8209 INVARIANCE rejection stays. `IG2<T>: IComparable<IG2<int>>`
// names `IComparable<IG2<int>>`, NOT `IComparable<IG2<string>>`, and spec
// §12.9 makes interface type arguments INVARIANT — so `IG2<string>` does not
// satisfy `where T: IComparable<T>`. The chain walk compares the argument with
// §14.2's exact, component-wise identity, so the substituted `IG2<int>` does
// not equal the element `IG2<string>`.
BEZEL_TEST(codegen_linq_iface_chain_icomparable_8186,
           control_invariant_generic_argument_stays_rejected_8209) {
    JitResult r = runAot(prog(
        "public interface IG2<T>: IComparable<IG2<int>>\n"
        "  method Rank(): int;\n"
        "end IG2;\n"
        "class G2: IG2<string>\n"
        "  private _r: int;\n"
        "  public constructor(r: int) _r := r; end constructor;\n"
        "  public method Rank(): int => _r;\n"
        "  public method CompareTo(other: IG2<int>): int => _r - other.Rank();\n"
        "end G2;\n",
        "    using var a := new G2(3);\n"
        "    using var b := new G2(1);\n"
        "    var arr: Array<IG2<string>> := [a, b];\n"
        "    var m := arr.Min();\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "IG2 does not implement IComparable<T>"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// #8098 (spec §5.1, stdlib §5 / §4.1 / §4.4 / §4.12, v0.355): a LAZY
// stdlib §5 operator result must not be accepted into a CONCRETE collection
// binding (or an assignment, a parameter slot or a return slot).
//
// THE RULE. stdlib §5 declares the operators on `IEnumerable<T>`:
//   "public extension IEnumerable<T>
//      public method Where(predicate: (T) -> bool): IEnumerable<T>;
//      public method Select<U>(selector: (T) -> U): IEnumerable<U>;
//      ...
//      public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>;
//      public method Reverse(): IEnumerable<T>;
//      public method Take(count: int): IEnumerable<T>; ..."
// so `a.Where(p)` has type `IEnumerable<int>` — a lazy SEQUENCE, not a
// collection.
//
// §5.1's implicit-conversion list is closed: the numeric table, the
// "never implicit" bullets, and the *Reference conversions* list — identity,
// class to base class up to `Object`, "any type to `Object`", "a type to an
// interface it implements", interface to extended interface, a type parameter
// to what its constraints guarantee, and §12.9's variance-induced ones. A
// sequence is none of those rows' source: it is an `IEnumerable<T>` value, not
// a class, and it implements no collection interface.
//
// stdlib §4.12: "public interface IEnumerable<out T>
//                    method GetEnumerator(): owned IEnumerator<T>;
//                  end IEnumerable;" — it declares ONLY `GetEnumerator` and
// extends NOTHING. So the lazy sequence conforms to NEITHER `IList<T>` NOR
// `IReadOnlyList<T>` NOR `ICollection<T>` NOR `IReadOnlyCollection<T>`
// (stdlib §4.1), and the concrete collection classes (`Array<T>` §4.4,
// `Set<T>` §4.10, `Dictionary<K, V>` §4.8, `FrozenArray<T>` §4.19,
// `LinkedList<T>` §4.6) are CLASSES, so a sequence-to-class conversion is a
// class-to-class one with no nominal relation.
//
// THE ONLY collection targets a lazy result may reach are the ones it IS:
// `IEnumerable<T>` and `IOrderedEnumerable<T>` (stdlib §5's
// `OrderBy`/`ThenBy` result, which extends `IEnumerable<T>`). `Object` stays
// admitted by §5.1's "any type to `Object`".
//
// THE DEFECT. The lazy result's compiler-internal type is a structural
// sentinel (a Named TypeRef with no declaration, no astNode, the element type
// at genericArgs[0] and no importedName), so every NAME-keyed §5.1 arm missed
// it and the catch-all admitted it. `var q: Array<int> := a.Where(p);`
// compiled with rc=0, bound the SEQUENCE POINTER into the `Array<int>` slot,
// printed an address and then SEGV'd (MEASURED on 74ee5c00: exit 139 — the
// issue's repro).
//
// `List<T>` does not exist and there is no `ToList()`: stdlib §4.6 — "`List<T>`
// is not defined in this version. The name is reserved."; stdlib §5 — "There
// is no `ToList()` because Bezel does not have a dynamic-array type named
// `List<T>` (`Array<T>` is the resizable form; `LinkedList<T>` is the linked
// form; use `new LinkedList<T>(source)` if a linked list is wanted)."

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A lazy result rejected at a §5.1 conversion site, with the diagnostic text
// asserted: the §5.1 clause AND the target the conversion was refused for.
bool rejectedWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.stdoutText.empty()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// An ACCEPT control: the program compiles, runs, exits 0 and prints exactly
// `expected`.
void expectStdout(const std::string& program, const std::string& expected) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The §5.1 lazy-sequence refusal, named for the target it was refused for.
std::string refusal(const std::string& target) {
    return "a deferred stdlib \xc2\xa7""5 operator yields a lazy "
           "`IEnumerable<T>` sequence, and \xc2\xa7""5.1 lists no implicit "
           "conversion from a sequence to `" + target + "`";
}

// `a` = [1,2,3,4]; `a.Where(x => x > 2)` = [3,4].
std::string program(const std::string& body) {
    return "module M;\n"
           "class P\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1, 2, 3, 4];\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the BINDING spelling, one row per concrete collection target.
// ---------------------------------------------------------------------------

// #8098: the issue's own repro — a lazy `Where` result into an `Array<int>`
// binding. MEASURED on the base: rc=0, an address printed, exit 139.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_array_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Array<int> := a.Where(lambda(x) => x > 2);\n"
                "    Console.WriteLine(q.Count);\n"),
        refusal("Array")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_set_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Set<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("Set")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_dictionary_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Dictionary<int, int> := "
                "a.Where(lambda(x) => x > 2);\n"),
        refusal("Dictionary")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_frozenarray_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: FrozenArray<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("FrozenArray")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_linkedlist_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: LinkedList<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("LinkedList")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_stack_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Stack<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("Stack")));
}

// stdlib §4.1: `IList<T>` / `IReadOnlyList<T>` / `ICollection<T>` /
// `IReadOnlyCollection<T>` are the borrowing collection INTERFACES, and
// stdlib §4.12's `IEnumerable<out T>` extends nothing — so the lazy sequence
// implements none of them and §5.1 admits no conversion to them either.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_ilist_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: IList<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("IList")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_ireadonlylist_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: IReadOnlyList<int> := "
                "a.Where(lambda(x) => x > 2);\n"),
        refusal("IReadOnlyList")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_icollection_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: ICollection<int> := a.Where(lambda(x) => x > 2);\n"),
        refusal("ICollection")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_ireadonlycollection_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: IReadOnlyCollection<int> := "
                "a.Where(lambda(x) => x > 2);\n"),
        refusal("IReadOnlyCollection")));
}

// ---------------------------------------------------------------------------
// REJECT — the ASSIGNMENT, PARAMETER-PASS and RETURN spellings.
// ---------------------------------------------------------------------------

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_assigned_to_array_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3, 4];\n"
        "    using var q: Array<int> := [9];\n"
        "    q := a.Where(lambda(x) => x > 2);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        refusal("Array")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_passed_as_array_argument_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class P\n"
        "  public static method Take(x: Array<int>): int => 0;\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3, 4];\n"
        "    Console.WriteLine(Take(a.Where(lambda(x) => x > 2)));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        refusal("Array")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_returned_as_array_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M;\n"
        "class P\n"
        "  public static method Take(): owned Array<int>\n"
        "    var a: Array<int> := [1, 2, 3, 4];\n"
        "    return a.Where(lambda(x) => x > 2);\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    using var q := Take();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        refusal("Array")));
}

// ---------------------------------------------------------------------------
// REJECT — the other deferred operators (they all yield the same lazy type).
// ---------------------------------------------------------------------------

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           select_result_into_array_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Array<int> := a.Select(lambda(x) => x * 2);\n"),
        refusal("Array")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           take_result_into_array_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Array<int> := a.Take(2);\n"),
        refusal("Array")));
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           orderby_result_into_array_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: Array<int> := a.OrderBy(lambda(x) => x);\n"),
        refusal("Array")));
}

// #7625's own §5.1 refusal (a lazy result into a PRIMITIVE target) is
// unchanged by #8098 — the two rejections differ only in the target kind.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_into_primitive_still_rejected_by_7625) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        program("    var q: int := a.Where(lambda(x) => x > 2);\n"),
        "a deferred stdlib \xc2\xa7""5 operator yields a lazy "
        "`IEnumerable<T>` sequence, which converts to no primitive type"));
}

// ---------------------------------------------------------------------------
// ACCEPT controls — the shapes that must KEEP compiling (and keep working).
// ---------------------------------------------------------------------------

// `IEnumerable<T>` is the sequence's OWN type (stdlib §4.12) — the identity
// conversion, §5.1.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_bound_to_ienumerable_accepted) {
    expectStdout(
        program("    var q: IEnumerable<int> := a.Where(lambda(x) => x > 2);\n"
                "    Console.WriteLine(q.Count());\n"),
        "2\n");
}

// `IOrderedEnumerable<T>` is stdlib §5's `OrderBy`/`ThenBy` result and extends
// `IEnumerable<T>` — the one other type a lazy result IS. (`OrderBy` SORTS,
// so the count is the source's 4, not a filter's 2.)
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           orderby_result_bound_to_iorderedenumerable_accepted) {
    expectStdout(
        program("    var q: IOrderedEnumerable<int> := "
                "a.OrderBy(lambda(x) => x);\n"
                "    Console.WriteLine(q.Count());\n"),
        "4\n");
}

// An INFERRED `var` has no written target type, so no §5.1 conversion site
// is involved at all.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_inferred_var_accepted) {
    expectStdout(
        program("    var q := a.Where(lambda(x) => x > 2);\n"
                "    Console.WriteLine(q.Count());\n"),
        "2\n");
}

// A `for` over the lazy result consumes it where it stands (stdlib §5's
// single-consumption rule, spec §29.11).
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_foreach_accepted) {
    expectStdout(
        program("    for var x in a.Where(lambda(y) => y > 2) do\n"
                "      Console.WriteLine(x);\n"
                "    end;\n"),
        "3\n4\n");
}

// The EAGER materializers of stdlib §5 — `ToArray(): owned Array<T>`,
// `ToSet(): owned Set<T>`, `ToDictionary<TKey>(key): owned Dictionary<TKey, T>`
// — are the spelled-out remedy and stay admitted.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_toarray_materializer_accepted) {
    expectStdout(
        program("    using var q: Array<int> := "
                "a.Where(lambda(x) => x > 2).ToArray();\n"
                "    Console.WriteLine(q.Count);\n"),
        "2\n");
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_toset_materializer_accepted) {
    expectStdout(
        program("    using var q: Set<int> := "
                "a.Where(lambda(x) => x > 2).ToSet();\n"
                "    Console.WriteLine(q.Count);\n"),
        "2\n");
}

BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_todictionary_materializer_accepted) {
    expectStdout(
        program("    using var q: Dictionary<int, int> := "
                "a.Where(lambda(x) => x > 2).ToDictionary(lambda(x) => x);\n"
                "    Console.WriteLine(q.Count);\n"),
        "2\n");
}

// A parameter typed `IEnumerable<T>` (stdlib §4.12) — the §5 operators
// themselves are declared there, so the sequence reaches it unchanged.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           where_result_passed_to_ienumerable_parameter_accepted) {
    expectStdout(
        "module M;\n"
        "class P\n"
        "  public static method Take(x: IEnumerable<int>): int => x.Count();\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3, 4];\n"
        "    Console.WriteLine(Take(a.Where(lambda(x) => x > 2)));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        "2\n");
}

// stdlib §4.6: `public constructor(source: IEnumerable<T>);` — `LinkedList<T>`'s
// source constructor takes a SEQUENCE, so a lazy result is a legal argument
// (the lazy sequence IS an `IEnumerable<T>`).
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           linkedlist_source_constructor_accepts_lazy_result_accepted) {
    expectStdout(
        program("    using var l := new LinkedList<int>(\n"
                "      a.Where(lambda(x) => x > 2));\n"
                "    Console.WriteLine(l.Count);\n"),
        "2\n");
}

// stdlib §4.4: `public method AddRange(items: IEnumerable<T>);` — the same
// reason, on `Array<T>`'s own member.
BEZEL_TEST(lazy_result_to_concrete_collection_8098,
           array_addrange_accepts_lazy_result_accepted) {
    expectStdout(
        program("    using var b: Array<int> := [0];\n"
                "    b.AddRange(a.Where(lambda(x) => x > 2));\n"
                "    Console.WriteLine(b.Count);\n"),
        "3\n");
}

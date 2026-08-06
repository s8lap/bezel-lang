// fix #2914 — a collection literal at a target OUTSIDE §15.4's closed target
// list was not rejected (spec §15.4 *Collection literals*, §15.1, §8.6 —
// v0.240).
//
// WHAT THE SPEC REQUIRES.  §15.4 states the rule positively and closes it:
//
//   "A collection literal has no standalone type. It is well-typed only in a
//    context that provides the target type, either via type annotation or via
//    the parameter type of a method call."
//
//   "The target types that admit collection literals are: Inline arrays `[N]T`,
//    `[N]T?`, `[N owned]T`, `[N owned]T?` … `Array<T>` … and `OwningArray<T>`
//    … `Set<T>` … `LinkedList<T>` … `Stack<T>` … `Queue<T>` …
//    `Dictionary<K, V>` … Any user-defined type declaring the
//    `FromCollectionLiteral` static factory".
//
// `Object` is not on that list, and neither is `int`, `string`, `Object?`, a
// plain class, or a bare type parameter.  §8.6 does not add one: it boxes a
// value *of* an inline-array type, it does not give an untyped literal one.
//
// MEASURED on f069645 (the compiler this fix was written against), all with
// NO user-facing diagnostic:
//
//   var a: Object := [1, 2, 3];   -> ICE: "Invalid indices for GEP pointer
//   var a: Object? := [1, 2, 3];      type!" x3, then "bezel codegen: IR
//   var a: int := [1, 2, 3];          verification failed" — the compile
//   var a: string := [1, 2, 3];       ABORTS with an LLVM verifier dump and
//   var a: K := [1, 2, 3];            no diagnostic naming the program's fault
//   var x: T := [1, 2, 3];            (T = a bare type parameter)
//
//   var o: Object := [];          -> compiles; `o is Object` reads FALSE;
//   var o: Object := ["a": 1];       exit 3 instead of 7 — a SILENTLY WRONG
//                                    value behind only the #2434 `internal:`
//                                    warning.
//
// THE CAUSE was the SHAPE of the gate, not a missing name in it.  The
// local-declaration position asked `isCollectionLiteralInterfaceTarget` — a
// NEGATIVE predicate listing the collection interfaces #1174 had found — so
// every non-admissible target that was not one of those names walked straight
// through into codegen's inline-array arm, which GEPs the (pointer- or
// scalar-typed) slot as an array.  §15.4's own rule is POSITIVE and CLOSED, so
// the fix asks the positive predicate (`isCollectionLiteralAdmissibleTarget`,
// which already existed for the lambda-body position, #1825): a target is
// admitted only by being ON the list.  The interface arm keeps its own, more
// specific message.
//
// ⚠ MAINTENANCE NOTE — the premise of the REJECT tests below can expire.  They
// pin that a target is NOT on §15.4's list.  If a later revision ADDS one of
// these types to §15.4's target list (say `Object` via an inferred inline-array
// type), the corresponding test must be re-pointed at whatever remains outside
// the list — NOT deleted, and NOT quietly flipped to an accept: the assertion
// being pinned is "the closed list is enforced", and it must keep failing
// loudly if it stops being.  The accept-controls in the other direction
// (`admissible_targets_still_materialize_spec_15_4`) are the guard that the
// closed list did not become an over-reject; they assert a distinctive NON-ZERO
// value precisely so "everything returned 0" cannot pass them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// True when the compile was rejected AND the rejection is §15.4's, naming the
// literal and the target type — not some unrelated downstream error and not the
// LLVM verifier abort this fix replaced.
bool rejectedByRule15_4(const JitResult& r, const std::string& spelledTarget) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("collection literal `[...]` cannot target") !=
                std::string::npos &&
            d.message.find("`" + spelledTarget + "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

JitResult decl(const std::string& declText) {
    return runAot("module M; class K public constructor() end constructor; end K; "
                  "class C public static method Main(): int " +
                  declText + " return 0; end Main; end C;");
}

}  // namespace

// =====================================================================
// #2914 — REJECT: every target outside §15.4's closed list.
// Pre-fix each of these ABORTED the compile with an LLVM verifier dump
// ("Invalid indices for GEP pointer type!") and no diagnostic.
// =====================================================================

BEZEL_TEST(sema_collection_literal_target_2914,
           object_target_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("var a: Object := [1, 2, 3];"), "Object"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           nullable_object_target_rejected_spec_15_4) {
    // The spelling in the message must be the grammar's postfix `T?` (§7), not
    // a prefixed `?Object` — a remedy that names a different type is no remedy.
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("var a: Object? := [1, 2, 3];"), "Object?"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           primitive_target_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("var a: int := [1, 2, 3];"), "int"));
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("var a: string := [1, 2, 3];"), "string"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           plain_class_target_rejected_spec_15_4) {
    // A class that does NOT declare `FromCollectionLiteral` — §15.4's user hook
    // is opt-in, so a class without it is not a target.
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("using var a: K := [1, 2, 3];"), "K"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           bare_type_parameter_target_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Make<T>(): int "
        "    var x: T := [1, 2, 3]; "
        "    return 0; "
        "  end Make; "
        "  public static method Main(): int return Make<Array<int>>(); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(r, "T"));
}

// =====================================================================
// #2914 — REJECT: the two forms that did NOT ICE pre-fix but produced a
// SILENTLY WRONG VALUE. These are the sharper half of the defect: no
// verifier abort, no error, just a zero-initialized `Object` slot.
// =====================================================================

BEZEL_TEST(sema_collection_literal_target_2914,
           empty_literal_at_object_target_rejected_spec_15_4) {
    // Pre-fix: compiled, `o is Object` read FALSE, program exited 3 not 7.
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(decl("var o: Object := [];"), "Object"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           dictionary_literal_at_object_target_rejected_spec_15_4) {
    // Pre-fix: compiled, `o is Object` read FALSE, program exited 3 not 7.
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(decl("var o: Object := [\"a\": 1, \"b\": 2];"),
                           "Object"));
}

// =====================================================================
// #2914 — the INTERFACE arm keeps its own, more specific message (#1174 /
// #1866). A control that the positive gate did not swallow it.
// =====================================================================

BEZEL_TEST(sema_collection_literal_target_2914,
           interface_target_keeps_its_specific_message_spec_15_4) {
    JitResult r = decl("var o: IEnumerable<int> := [1, 2, 3];");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot target the interface type `IEnumerable`") !=
                std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// =====================================================================
// #2914 — ACCEPT-CONTROLS: §15.4's list is enforced, not narrowed.
// Every admissible target still materializes AND still carries its values.
// The expected value is deliberately a distinctive non-zero number so a
// program that compiled but produced nothing cannot pass.
// =====================================================================

BEZEL_TEST(sema_collection_literal_target_2914,
           admissible_targets_still_materialize_spec_15_4) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var ia: [3]int := [1, 2, 3]; "
        "    var na: [3]int? := [4, 5, 6]; "
        "    var ar: Array<int> := [7, 8, 9]; "
        "    var st: Set<int> := [1, 2, 3]; "
        "    var sk: Stack<int> := [1, 2, 3]; "
        "    var qu: Queue<int> := [1, 2, 3]; "
        "    var ll: LinkedList<int> := [1, 2, 3]; "
        "    var di: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    return ia[0] + ar[1] + st.Count + sk.Count + qu.Count + ll.Count "
        "         + di.Count + (na[2] ?? 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 1 + 8 + 3 + 3 + 3 + 3 + 2 + 6 = 29 — every target contributes a
    // different addend, so a single dropped materialization changes the sum.
    BEZEL_EXPECT_EQ(r.exitCode, 29);
}

BEZEL_TEST(sema_collection_literal_target_2914,
           owning_heap_target_still_materializes_spec_15_4) {
    // 2*100 + 5*10 + 9 = 259, which is OUTSIDE the 8-bit exit status — so the
    // observable goes to stdout via runAotOut. (Asserting it as an exit code
    // read 3, i.e. 259 & 0xFF, which is exactly the truncation trap: a wrong
    // value that looks like a plausible small answer.)
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class R public N: int; "
                  "  public constructor(n: int) N := n; end constructor; end R; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var oa: OwningArray<R> := [new R(5), new R(9)]; "
                  "    Console.WriteLine(oa.Count * 100 + oa[0].N * 10 "
                  "                      + oa[1].N); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("259"));
}

BEZEL_TEST(sema_collection_literal_target_2914,
           inline_array_target_inside_a_generic_still_accepted_spec_15_4) {
    // `var x: Array<T> := []` inside a generic body: the head name is on
    // §15.4's list even though the element is still a type parameter, so the
    // positive gate must admit it. Measured green pre-fix; a false reject here
    // would be the gate over-narrowing.
    JitResult r = runAot(
        "module M; "
        "class Box<T> "
        "  public static method Make(): int "
        "    var x: Array<T> := []; "
        "    return x.Count + 41; "
        "  end Make; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box<int>.Make(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

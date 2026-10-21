// Sema — #6814 / language spec §27.6 (v0.340) + stdlib §5 (`IEnumerable<T>`
// operators), §4 (the collections), §7.5 (`?.`).
//
// A bare reference to a stdlib §5 operator the compiler dispatches
// structurally (no method symbol) cannot form a function value. #6784 rejected
// it on IGrouping and on an IEnumerable<T> `.`; #6805 on string. Before the
// fix the remaining spellings (`?.` on every builtin collection; `.` on
// IList / IReadOnlyList / ICollection / IReadOnlyCollection / Span /
// ReadOnlySpan / IOwning*) were left untyped, codegen degraded the local to a
// ZERO function value (warning #2434) and calling it SEGV'd. Each is now a
// located compile-time rejection. The call spelling, user declarations and
// the declared `First` / `Last` PROPERTIES of the linked lists and
// SortedSet<T> (stdlib §4.7, §4.18) stay accepted.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kPre =
    "module M; public class M public static method Main(): int "
    "  var a: Array<int> := [1, 2, 2, 3]; ";
constexpr const char* kPost = " end Main; end M;";

void expectRejected(const std::string& body, const std::string& member,
                    const std::string& recv) {
    JitResult r = runAot(std::string(kPre) + body + kPost);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("a bare reference to `" + member + "` (language spec \xC2\xA7"
            "27.6) is not yet supported on `" + recv + "`").c_str()));
}

// `var f := <recvVar>.<member>;` over a local of type `ty` bound to `a`.
void dot(const std::string& ty, const std::string& member,
         const std::string& recv) {
    expectRejected("var e: " + ty + " := a; var f := e." + member +
                       "; return 0;",
                   member, recv);
}
void nullCond(const std::string& ty, const std::string& member,
              const std::string& recv) {
    expectRejected("var e: " + ty + "? := a; var f := e?." + member +
                       "; return 0;",
                   member, recv);
}

}  // namespace

// ---- `?.` (the verbatim item-2 repro first) ----
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_ienumerable_first_rejected_spec_27_6) {
    nullCond("IEnumerable<int>", "First", "IEnumerable<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_array_first_rejected_spec_27_6) {
    nullCond("Array<int>", "First", "Array<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_ilist_first_rejected_spec_27_6) {
    nullCond("IList<int>", "First", "IList<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_ireadonlylist_where_rejected_spec_27_6) {
    nullCond("IReadOnlyList<int>", "Where", "IReadOnlyList<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_icollection_sum_rejected_spec_27_6) {
    nullCond("ICollection<int>", "Sum", "ICollection<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_stack_first_rejected_spec_27_6) {
    expectRejected("var e: Stack<int>? := new Stack<int>(); var f := e?.First; "
                   "return 0;",
                   "First", "Stack<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_set_toarray_rejected_spec_27_6) {
    expectRejected("var e: Set<int>? := new Set<int>(); var f := e?.ToArray; "
                   "return 0;",
                   "ToArray", "Set<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_dictionary_first_rejected_spec_27_6) {
    expectRejected("var e: Dictionary<int, int>? := new Dictionary<int, int>(); "
                   "var f := e?.First; return 0;",
                   "First", "Dictionary<K, V>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_sortedset_where_rejected_spec_27_6) {
    expectRejected("var e: SortedSet<int>? := new SortedSet<int>(); "
                   "var f := e?.Where; return 0;",
                   "Where", "SortedSet<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           nullcond_linkedlist_where_rejected_spec_27_6) {
    expectRejected("var e: LinkedList<int>? := new LinkedList<int>(); "
                   "var f := e?.Where; return 0;",
                   "Where", "LinkedList<T>");
}

// ---- `.` on the collection interfaces and spans ----
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           dot_ilist_first_rejected_spec_27_6) {
    dot("IList<int>", "First", "IList<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           dot_ireadonlylist_first_rejected_spec_27_6) {
    dot("IReadOnlyList<int>", "First", "IReadOnlyList<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           dot_icollection_first_rejected_spec_27_6) {
    dot("ICollection<int>", "First", "ICollection<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           dot_ireadonlycollection_sum_rejected_spec_27_6) {
    dot("IReadOnlyCollection<int>", "Sum", "IReadOnlyCollection<T>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           dot_readonlyspan_first_rejected_spec_27_6) {
    expectRejected("var e: ReadOnlySpan<int> := a.AsReadOnlySpan(); "
                   "var f := e.First; return 0;",
                   "First", "ReadOnlySpan<T>");
}

// ---- Controls: accepted spellings keep exact output ----
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           call_forms_print_1_1_1_spec_5) {
    JitResult r = runAot(std::string(kPre) +
        "var e: Array<int>? := a; var l: IList<int> := a; "
        "Console.WriteLine(e?.First() ?? -1); "
        "Console.WriteLine(l.First()); "
        "Console.WriteLine((l.First)()); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n1\n1\n"));
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_non_operator_name_prints_7_spec_26_5) {
    JitResult r = runAot(
        "module M; extension IEnumerable<int> "
        "public method First2(): int return 7; end First2; end; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; var e: IEnumerable<int> := a; "
        "  var f: () -> int := e.First2; Console.WriteLine(f()); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_class_first_member_reference_prints_5_spec_27_6) {
    JitResult r = runAot(
        "module M; public class Box public method First(): int return 5; "
        "end First; end Box; public class M public static method Main(): int "
        "  var b := new Box(); var c: Box? := b; "
        "  var f: () -> int := b.First; Console.WriteLine(f()); "
        "  var g: () -> int := c!.First; Console.WriteLine(g()); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n5\n"));
}
// The declared linked-list `First` property is not a §5 operator.
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           linkedlist_dot_first_property_prints_7_spec_4_7) {
    JitResult r = runAot(std::string(kPre) +
        "var l := new LinkedList<int>(); l.AddLast(7); "
        "Console.WriteLine(l.First ?? -1); return 0;" + kPost);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ---- A same-named user extension wins (§26.5 rules 2-3, §27.6) ----
// §27.6 admits a receiver-qualified extension-method reference, "the member
// resolved on the value exactly as the call `v.Scaled()` is (§26.5)". A user
// `extension IList<int>` is more specific than §5's `extension
// IEnumerable<T>` at an `IList<int>` receiver (§26.5.1), so `e.Last` names
// the user member and is not the structural-operator rejection. The rejected
// first cut of #6814 reported §27.6 on these before extension lookup.
namespace {
JitResult runUserLast(const std::string& head, const std::string& body) {
    return runAot(
        "module M; extension " + head +
        " public method Last(a: int): int return a + 1; end Last; end; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; " + body + " return 0; "
        "end Main; end M;");
}
void expectUserLastPrints6(const std::string& ty) {
    JitResult r = runUserLast(
        ty, "var e: " + ty + " := a; var g: (int) -> int := e.Last; "
            "Console.WriteLine(g(5));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
}  // namespace

BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ilist_last_reference_prints_6_spec_26_5) {
    expectUserLastPrints6("IList<int>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ireadonlylist_last_reference_prints_6_spec_26_5) {
    expectUserLastPrints6("IReadOnlyList<int>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_icollection_last_reference_prints_6_spec_26_5) {
    expectUserLastPrints6("ICollection<int>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ireadonlycollection_last_reference_prints_6_spec_26_5) {
    expectUserLastPrints6("IReadOnlyCollection<int>");
}
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_span_last_reference_prints_6_spec_26_5) {
    JitResult r = runUserLast(
        "Span<int>", "var e: Span<int> := a.AsSpan(); "
                     "var g: (int) -> int := e.Last; Console.WriteLine(g(5));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
// An inferred target: the user member's own signature is the formed type.
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ilist_inferred_reference_prints_6_spec_27_6) {
    JitResult r = runUserLast(
        "IList<int>", "var e: IList<int> := a; var g := e.Last; "
                      "Console.WriteLine(g(5));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
// Control: a non-§5 name on IList was never the rejection's business.
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ilist_non_operator_name_prints_6_spec_26_5) {
    JitResult r = runAot(
        "module M; extension IList<int> "
        "public method Foo(a: int): int return a + 1; end Foo; end; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; var e: IList<int> := a; "
        "  var g: (int) -> int := e.Foo; Console.WriteLine(g(5)); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}
// An extension on a DIFFERENT instantiation does not apply to `IList<int>`
// (§26.5 rule 2's unification), so both spellings stay rejected.
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_other_element_type_still_rejected_spec_27_6) {
    for (const char* body :
         {"var e: IList<int> := a; var g: (int) -> int := e.Last; ",
          "var e: IList<int>? := a; var g := e?.Last; "}) {
        JitResult r = runUserLast("IList<string>", body);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(
            r, "a bare reference to `Last` (language spec \xC2\xA7"
               "27.6) is not yet supported on `IList<T>`"));
    }
}
// §26.5.1's own example: a user `extension IEnumerable<int>` member `First()`
// and §5's `extension IEnumerable<T>` `First()` have identical substituted
// heads, so the reference, like the call, is §26.5 rule 4's ambiguity, not
// the structural-operator rejection (narrow #6784: the `.` IEnumerable arm
// reported §27.6 before extension lookup).
BEZEL_TEST(collection_bare_operator_reference_reject_6814,
           user_extension_ienumerable_first_reference_ambiguous_spec_26_5_1) {
    JitResult r = runAot(
        "module M; extension IEnumerable<int> "
        "public method First(): int return 9; end First; end; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; var e: IEnumerable<int> := a; "
        "  var g: () -> int := e.First; Console.WriteLine(g()); return 0; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous extension member `First`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "a bare reference to `First` (language spec"));
}

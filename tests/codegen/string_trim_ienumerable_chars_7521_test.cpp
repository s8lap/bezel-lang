// Fix #7521 -- `string.Trim(chars: IEnumerable<char>)` refused every
// `IEnumerable<char>` source other than `string` and `Array<char>`, and the
// runtime helper could read only an `Array<char>` (stdlib §9.6, §5.1,
// v0.355).
//
// THE DECLARED FORM. stdlib §9.6 (v0.355), the `String` class:
//
//     public pure method Trim(): string;
//     public pure method Trim(chars: IEnumerable<char>): string;
//     public pure method TrimStart(): string;
//     public pure method TrimEnd(): string;
//
// `TrimStart`/`TrimEnd` declare NO parameter, so the `chars` position is
// `Trim`'s alone -- there is no `TrimStart(chars)`/`TrimEnd(chars)` to wire.
//
// THE DEFECT (two halves).
//
//   1. SEMA. The §9.6 admission gate read the §5.1 conformance question
//      ("a type to an interface it implements or conforms to") off a
//      HAND-WRITTEN list -- `string` and `Array<char>` only. Every other
//      `IEnumerable<char>` source was refused with "no matching overload of
//      `string.Trim` -- the `chars` argument must be an `IEnumerable<char>`",
//      while the general §5.1 machinery already admitted those very types at
//      an `IEnumerable<char>` position: MEASURED on the base (v0.355,
//      commit f9e23eba), `var e: IEnumerable<char> := s;` compiles while
//      `s.Trim(e)` is REFUSED -- the same program, two answers, one of them
//      a §5.1 identity the language does not deny.
//
//   2. CODEGEN. The runtime helper `bz_str_trim_chars` reads its set as an
//      `Array<char>` (a `bz_array_t` whose slots are i32 code points,
//      `bz_cp_in_set`), so admitting a source was not enough: a `Set<T>`/
//      `SortedSet<T>` is `bz_dict_t*`-backed, a `LinkedList<T>` is
//      `bz_llist_t*`-backed, a lazy `.Where(...)` pipeline and an
//      interface-typed `IEnumerable<char>` are enumerator objects. Reading
//      one of those as an array header is the #7492 shape (a SIGSEGV).
//
// THE FIX.
//
//   * SEMA: the conformance question is asked of the ONE §26.5 builtin
//     conformance table (`ExprTC::builtinArgIsEnumerableCharSource`), so a
//     `Set<char>`/`Stack<char>`/`Queue<char>`/`LinkedList<char>` and a lazy
//     `.Where(...)` pipeline are admitted at §9.6's `IEnumerable<char>`
//     position -- and the gate cannot disagree with the general §5.1
//     machinery about a builtin collection.
//
//   * CODEGEN: EVERY source is MATERIALISED into a fresh `Array<char>`
//     snapshot through the ONE generic IEnumerable path
//     (`lowerToLazyEnumerator` + `bz_lazy_to_array`) -- the same mechanism
//     stdlib §9.6's `string.Join(separator, items: IEnumerable<string>)`
//     uses for its `items` argument. No collection is special-cased: the ONE
//     path enumerates whatever the argument is.
//
// THE SOURCE IS BORROWED (§28.9): the snapshot is the call's own and is
// released once the trim has read it; the caller's collection is NEVER
// disposed by the call.
//
// ORACLE. `runAot` for the behaviour rows; `runAotChecked` (generated-code
// ASan + LSan) for the lazy-source rows.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return std::string(
               "module M; import System;\n"
               "class C\n"
               "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// THE ADMITTED SOURCES -- each one trims with the issue's own repro.
// ═══════════════════════════════════════════════════════════════════════════

// stdlib §9.6: `Trim(chars: IEnumerable<char>): string`. A `Set<char>` is an
// `IEnumerable<char>` (stdlib §4.10: `Set<T>: ICollection<T>`, and
// `ICollection<T>` extends `IReadOnlyCollection<T>`, which extends
// `IEnumerable<T>`), so §5.1 converts it to the declared parameter type.
// The issue's repro: `"abxba".Trim(set{a})` is `bxb`.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           set_char_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var s := new Set<char>();\n"
        "    s.Add('a');\n"
        "    Console.WriteLine(\"abxba\".Trim(s));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §4.7: `Stack<T>: IReadOnlyCollection<T>` -- the read-only chain
// reaches `IEnumerable<T>` (stdlib §4.5).
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           stack_char_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var s := new Stack<char>();\n"
        "    s.Push('a');\n"
        "    Console.WriteLine(\"abxba\".Trim(s));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §4.7: `Queue<T>: IReadOnlyCollection<T>`.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           queue_char_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var s := new Queue<char>();\n"
        "    s.Enqueue('a');\n"
        "    Console.WriteLine(\"abxba\".Trim(s));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §4.6: `LinkedList<T>: ICollection<T>` -- a `bz_llist_t*`-backed
// source, whose runtime representation is NOT an array.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           linkedlist_char_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var s := new LinkedList<char>();\n"
        "    s.Add('a');\n"
        "    Console.WriteLine(\"abxba\".Trim(s));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §5: a LAZY `.Where(...)` pipeline over an `Array<char>`. The
// pipeline's runtime value is an enumerator object (a `bz_lazy_t*`), not an
// array -- and the compiler's own static type for it is the lazy sentinel,
// which the §9.6 gate used to spell as `?` ("the argument has type `?`").
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           lazy_where_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var a: Array<char> := ['a'];\n"
        "    Console.WriteLine(\"abxba\".Trim(a.Where(lambda(c) => c = 'a')));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §4.4: an argument whose OWN type is `IEnumerable<char>` -- the §5.1
// IDENTITY at the declared parameter type. On the base this was refused
// ("the argument has type `IEnumerable`") while `var e: IEnumerable<char> :=
// a;` compiled: the same program, two answers.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           ienumerable_char_variable_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var a: Array<char> := ['a'];\n"
        "    var e: IEnumerable<char> := a;\n"
        "    Console.WriteLine(\"abxba\".Trim(e));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// CONTROLS -- the two sources the base already admitted, still correct.
//
// stdlib §4.4: `Array<T>` is an `IEnumerable<T>`.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           array_char_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var a: Array<char> := ['a'];\n"
        "    Console.WriteLine(\"abxba\".Trim(a));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// stdlib §9.6: a `string` is an `IEnumerable<char>` (`public class String:
// IEnumerable<char>, ...`), and its representation is a `bz_string_struct`,
// which the string-specific helper walks in the code-point domain (§4.6).
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           string_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"abxba\".Trim(\"a\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// A set with a code point that is NOT at either end changes nothing: the
// trim strips only the LEADING and TRAILING code points that are in the set.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           interior_code_point_is_not_stripped_spec_9_6) {
    JitResult r = runAot(prog(
        "    var s := new Set<char>();\n"
        "    s.Add('x');\n"
        "    Console.WriteLine(\"abxba\".Trim(s));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "abxba");
}

// stdlib §4.4–§4.5: an argument whose own type is a BORROWING LIST interface
// -- `IList<char>` / `IReadOnlyList<char>` are array-backed (stdlib §4.5),
// so the ONE materialisation path reads them like an array.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           list_interface_source_trims_spec_9_6) {
    JitResult r = runAot(prog(
        "    var a: Array<char> := ['a'];\n"
        "    var l: IList<char> := a;\n"
        "    Console.WriteLine(\"abxba\".Trim(l));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bxb");
}

// ═══════════════════════════════════════════════════════════════════════════
// THE #853 FENCE -- a source that is an `IEnumerable<char>` but has NO
// lowering. It must be DIAGNOSED, never silently dropped.
// ═══════════════════════════════════════════════════════════════════════════

// stdlib §4.1–§4.3: an `ICollection<char>` IS an `IEnumerable<char>`
// (`ICollection<T>` extends `IReadOnlyCollection<T>`, which extends
// `IEnumerable<T>`), so §5.1 admits the type and so does the §9.6 gate.
//
// But the interface ERASES the concrete backing -- an `ICollection<char>`
// may hold an array, a linked list or a hash table -- so no single runtime
// dispatch is sound and the compiler has no materialisation for it (#853).
//
// THE ORACLE IS "THE COMPILE FAILS", never "the program runs": with the
// materialisation arm returning nothing, the enclosing statement emitted no
// code at all, the program ran, and it printed NOTHING (stdlib §21.9) --
// a silent drop, the worst outcome. A diagnostic is the only acceptable
// answer for a source the compiler cannot honour.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           polymorphic_collection_interface_is_diagnosed_spec_4_1) {
    JitResult r = runAot(prog(
        "    var s := new Set<char>();\n"
        "    s.Add('a');\n"
        "    var c: ICollection<char> := s;\n"
        "    Console.WriteLine(\"abxba\".Trim(c));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasError(r, "cannot be materialized into a trim set"));
}

// ═══════════════════════════════════════════════════════════════════════════
// THE ADMITTED SOURCES -- under ASan (generated-code ASan + LSan).
// ═══════════════════════════════════════════════════════════════════════════
// The LAZY source under the checked oracle: the pipeline is materialised into
// a snapshot and the snapshot is released, so 200 iterations must not leak
// and must not double-free.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           lazy_where_source_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    var a: Array<char> := ['a'];\n"
        "    var i := 0;\n"
        "    var n := 0;\n"
        "    while i < 200 do\n"
        "      n := n + \"abxba\".Trim(a.Where(lambda(c) => c = 'a')).Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "600");
}

// The `Set<char>` source under the checked oracle: the call BORROWS the
// caller's collection (§28.9), so the set is still the caller's to release
// and the call must neither free it nor leak the snapshot it made.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           set_char_source_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    var s := new Set<char>();\n"
        "    s.Add('a');\n"
        "    var i := 0;\n"
        "    var n := 0;\n"
        "    while i < 200 do\n"
        "      n := n + \"abxba\".Trim(s).Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "600");
}

// ═══════════════════════════════════════════════════════════════════════════
// THE REFUSALS THAT STAY -- §5.1 and §7.3 are unchanged.
// ═══════════════════════════════════════════════════════════════════════════

// §5.1 converts no integer type to an `IEnumerable<char>`.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           int_char_source_is_refused_spec_5_1) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"abxba\".Trim(5));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "no matching overload of `string.Trim`"));
    BEZEL_EXPECT_TRUE(
        hasError(r, "the `chars` argument must be an `IEnumerable<char>`"));
}

// A `char` is not an `IEnumerable<char>` either.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           char_char_source_is_refused_spec_5_1) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"abxba\".Trim('a'));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "no matching overload of `string.Trim`"));
}

// stdlib §4.4 makes an `Array<T>` an `IEnumerable<T>` -- with the element
// type it names, not a different one.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           int_array_char_source_is_refused_spec_5_1) {
    JitResult r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    Console.WriteLine(\"abxba\".Trim(a));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "no matching overload of `string.Trim`"));
}

// §7.3: the `null` literal reaches no non-nullable parameter.
BEZEL_TEST(string_trim_ienumerable_chars_7521,
           null_char_source_is_refused_spec_7_3) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"abxba\".Trim(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "the argument to `string.Trim` is nullable"));
}

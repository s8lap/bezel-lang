// fix #6530 (§3.6 rule 2 / §14.1, v0.355): #6500's range-index guard
// over-rejects a TYPE PARAMETER named after a reserved builtin.
//
// THE DEFECT. #6500 added a sema guard (src/sema/ExprTypecheck_Exprs.cpp, the
// `isDict` branch of the IndexExpr typecheck) that rejects a `Range` index on a
// dictionary whose key type is not a `Range<T>`. The guard is right about the
// real builtin case — stdlib §4.8/§4.9/§4.17 declare exactly ONE indexer each,
// `public property [key: K]: V`, and §23.4 admits an index only when it fits a
// DECLARED indexer's parameter-type list, so `d[1..5]` on a
// `Dictionary<int, V>` is not an index access at all.
//
// But the guard's EXEMPTION for a bare type parameter key (`Dictionary<T, V>` in
// a generic body) was decided by a NAME-keyed test: a declaration-less,
// single-part `Named` TypeRef is "a reserved builtin, or a type parameter"
// (BuiltinRegistry.hpp), so `common::isReservedBuiltinTypeName(headName())`
// was read as the project's own test for which one it is. The name cannot tell
// them apart. A type parameter named `Range` (or `Guid`) was therefore read as
// the BUILTIN and the guard fired on valid generic code:
//
//     public method F<Range>(k: Range): int
//       var d: Dictionary<Range, int> := [k: 7];
//       d[1 .. 5] := 9;      // REJECTED — "range index `[a..b]` is not
//       return d[k];         //   admitted on `Dictionary<K, V>`"
//     end F;
//
// while the same body under `F<T>` compiles. The asymmetry is the defect: both
// are equally undecidable at the DECLARATION.
//
// THE SPEC. §3.6 rule 2 reads an interior use of a generic body at each
// instantiation the declaration forms: at `Range := Range<int>` the index below
// is a §8.3 auto-hashable key lookup (stdlib §9.8) and it is admitted; at
// `Range := int` it is inadmissible. The decision therefore belongs to the
// instantiation, not to the declaration, and the declaration must fail OPEN.
// §14.1 is the source of the answer: the type parameters in scope at a
// declaration are the enclosing type's and the enclosing method's own, so
// "is this name a TYPE PARAMETER?" is a question about the SCOPE, not about
// the spelling.
//
// THE FIX. The bare-type-parameter test is now ONE helper that asks the
// SCOPE (`ExprTC::isBareTypeParamName`, §14.1) whether the name is a declared
// type parameter of the enclosing method, type or extension block, with the
// name-keyed registry test surviving only as the fallback for a name the
// scope does NOT declare — where it still keeps #6500's own rule that a
// MISSED built-in name costs a rejection that does not fire rather than one
// that fires falsely.
//
// The same name-keyed test stood at two MORE guards in the same file, and
// both over-rejected the same shape (rows below): the `in` operator's §5.1
// key-slot check and the index-argument check's unsubstituted-parameter walk.
//
// Every row below is on the `runAot` oracle.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A whole program, for the rows that need declarations of their own.
std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// The rejection oracle, in the shape #3892 asks for: `r.ran == false` ALONE is
// satisfied by a crash, a signal, a timeout and an ASan report as well as by a
// genuine rejection. What discriminates is the WORDING — this message does not
// exist anywhere in the pre-fix compiler's output for these programs, because
// on the pre-fix compiler these programs are REJECTED with it.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// stdout of a program that must compile, run and exit 0. `ran == false` reads
// back as a marker no expectation equals, so a rejection FAILS an accept row
// rather than passing it.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── The issue's own repro, verbatim ────────────────────────────────────────
//
// The probe the b9 verifier ran against the shared bezelc @95fe673f: the
// `F<Range>` body is rejected with #6500's range-index message, the `G<T>`
// body is accepted. On the fixed compiler BOTH are accepted, because the two
// are equally undecidable at the declaration.
BEZEL_TEST(dict_range_index_6530,
           issue_repro_type_parameter_named_range_is_admitted_spec_14_2) {
    auto r = runAot(
        "module P;\n"
        "public class U\n"
        "  public static method F<Range>(k: Range): int\n"
        "    var d: Dictionary<Range, int> := [k: 7];\n"
        "    d[1 .. 5] := 9;\n"
        "    return d[k];\n"
        "  end F;\n"
        "  public static method G<T>(k: T): int\n"
        "    var d: Dictionary<T, int> := [k: 7];\n"
        "    d[1 .. 5] := 9;\n"
        "    return d[k];\n"
        "  end G;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    // Pre-fix: rejected with "range index `[a..b]` is not admitted on
    // `Dictionary<K, V>`" at the `d[1 .. 5] := 9;` line of `F<Range>`.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("ok\n"), r.stdoutText);
}

// ── A GENERIC METHOD whose type parameter is named after a builtin ─────────
//
// `Helper.F<Range>` is the same declaration as the repro, and it is
// INSTANTIATED at the builtin `Range<int>` — the one instantiation at which the
// body is genuinely admissible (§8.3 / stdlib §9.8: `Range<T>` is a
// compiler-intrinsic auto-hashable record, so `d[1..5]` is an ordinary key
// lookup). The body is the #6500 suite's own three-key shape — three keys
// sharing a `Start` and differing in the other storage words (§16.3's
// `IsEndInclusive`, §16.4's `Step`) — read back through a literal, so the row
// is not satisfiable by an encoder that consults only the first storage word.
// `7 + 80 + 900 = 987`.
BEZEL_TEST(dict_range_index_6530,
           generic_method_type_parameter_named_range_is_admitted_spec_14_2) {
    BEZEL_EXPECT_EQ(
        std::string("987\n"),
        out("    Console.WriteLine(Helper.F<Range<int>>().ToString());\n"
            "    return 0;\n",
            "class Helper\n"
            "  public static method F<Range>(): int\n"
            "    var d := new Dictionary<Range, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    return d[1..5] + d[1..<5] * 10 + d[1..5 step 2] * 100;\n"
            "  end F;\n"
            "end Helper;\n"));
}

// ── A GENERIC TYPE whose type parameter is named after a builtin ───────────
//
// The same shape one level out: the type parameter is the enclosing CLASS's
// (§14.1: the type parameters in scope at a member declaration are the
// enclosing type's plus the enclosing method's own), so the scope question is
// a different one from the generic-method row above — the method's own
// parameter list is empty here. Instantiated at the builtin `Range<int>`.
BEZEL_TEST(dict_range_index_6530,
           generic_type_type_parameter_named_range_is_admitted_spec_14_2) {
    BEZEL_EXPECT_EQ(
        std::string("987\n"),
        out("    using var b := new Box<Range<int>>();\n"
            "    Console.WriteLine(b.F().ToString());\n"
            "    return 0;\n",
            "class Box<Range>\n"
            "  public method F(): int\n"
            "    var d := new Dictionary<Range, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    return d[1..5] + d[1..<5] * 10 + d[1..5 step 2] * 100;\n"
            "  end F;\n"
            "end Box;\n"));
}

// ── A type parameter named after a NON-range builtin ───────────────────────
//
// `Guid` is in the reserved-builtin set too (stdlib §2.6), so the name-keyed
// test read it as the builtin exactly as it read `Range`. The body here has NO
// valid instantiation (at `Guid := Guid` the range index is genuinely
// inadmissible), so this row is a DECLARATION-level row: it must not be
// rejected at the declaration, where §3.6 rule 2 defers the question to the
// instantiations.
BEZEL_TEST(dict_range_index_6530,
           type_parameter_named_guid_is_admitted_spec_14_2) {
    auto r = runAot(
        "module P;\n"
        "public class U\n"
        "  public static method F<Guid>(k: Guid): int\n"
        "    var d: Dictionary<Guid, int> := [k: 7];\n"
        "    d[1 .. 5] := 9;\n"
        "    return d[k];\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("ok\n"), r.stdoutText);
}

// ── CONTROLS: the real builtin case must STAY guarded ──────────────────────

// The control for the fix: `Guid` as the REAL builtin key type (not a type
// parameter) is still rejected. stdlib §4.8 declares exactly ONE indexer on
// `Dictionary` — `public property [key: K]: V` — and §23.4 admits an index
// only when it fits a declared indexer's parameter-type list, so a
// `Range<int>` is not a `Guid` and the expression has no meaning. This is the
// row a "just fail open on every bare key" fix would break.
BEZEL_TEST(dict_range_index_6530,
           control_real_builtin_guid_key_is_still_guarded_spec_23_4) {
    auto r = runAot(prog("    var d := new Dictionary<Guid, string>();\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// The same control INSIDE a generic body — the shape the fix touches. The
// enclosing method's own type parameter (`T`) is in scope, but the KEY type is
// the real `Guid` builtin and is not a type parameter, so the guard must still
// fire. A fix that read "we are in a generic scope" instead of "this name is a
// declared type parameter in scope" would fail this row.
BEZEL_TEST(dict_range_index_6530,
           control_real_builtin_guid_key_in_a_generic_body_is_still_guarded) {
    auto r = runAot(prog("    Console.WriteLine(Helper.F<int>(3).ToString());\n"
                         "    return 0;\n",
                         "class Helper\n"
                         "  public static method F<T>(k: T): int\n"
                         "    var d := new Dictionary<Guid, int>();\n"
                         "    d[1 .. 5] := 9;\n"
                         "    return 0;\n"
                         "  end F;\n"
                         "end Helper;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// ── The SAME predicate, at the two SIBLING guards ──────────────────────────
//
// The bare-type-parameter test that #6500 got wrong was a NAME-keyed one, and
// the same name-keyed test stood at two more guards in the same file. Both
// over-rejected a type parameter named after a reserved builtin, in exactly
// the same shape — MEASURED on the pre-fix base, with the type parameter
// named `T` instead of `Range`, BOTH are accepted:
//
//   * the `in` operator's §5.1 key-slot check (#6527/#6535) —
//     `method F<Range>(d: Dictionary<Range, int>): bool  return (0..1) in d;`
//     is rejected with "the left operand of `in` over `Dictionary` has a type
//     that is not implicitly convertible to the target type (§5.1)";
//
//   * the index-argument check's unsubstituted-parameter walk (#7064/#6319) —
//     `method F<Range>(d: Dictionary<Range, int>): int  return d[0];` is
//     rejected with "a value of primitive type `int` does not implicitly
//     convert to the type parameter `Range`".
//
// Both are the §3.6 rule 2 question — undecidable at the declaration — so
// both must fail OPEN. These two rows pin that; the two controls below pin
// that the real builtin case is still guarded at both sites.

// The `in` operator over a dictionary whose key type is a type parameter
// NAMED AFTER A BUILTIN. At the declaration the key type is undecidable, so
// the §5.1 verdict belongs to the instantiation.
BEZEL_TEST(dict_range_index_6530,
           in_operator_over_a_type_parameter_named_range_is_admitted_spec_14_2) {
    auto r = runAot(prog("    Console.WriteLine(\"ok\");\n"
                         "    return 0;\n",
                         "class Helper\n"
                         "  public static method F<Range>(d: Dictionary<Range, "
                         "int>): bool\n"
                         "    return (0 .. 1) in d;\n"
                         "  end F;\n"
                         "end Helper;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("ok\n"), r.stdoutText);
}

// The control for the `in` operator: a REAL key type whose §5.1 verdict is
// decidable at the declaration is still rejected. `string` does not convert to
// `int` (§5.1's table is numeric widening only).
BEZEL_TEST(dict_range_index_6530,
           control_in_operator_over_a_real_key_type_is_still_guarded_spec_5_1) {
    auto r = runAot(prog("    Console.WriteLine(\"ok\");\n"
                         "    return 0;\n",
                         "class Helper\n"
                         "  public static method F<T>(d: Dictionary<int, int>): "
                         "bool\n"
                         "    return \"x\" in d;\n"
                         "  end F;\n"
                         "end Helper;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r,
        "the left operand of `in` over `Dictionary` has a type that is not "
        "implicitly convertible to the target type"));
}

// The index-argument check against a key slot whose type is a type parameter
// NAMED AFTER A BUILTIN. The index argument (`0`) and the declared key type
// (the type parameter) are not comparable at the declaration, so the §5.1
// verdict belongs to the instantiation.
BEZEL_TEST(dict_range_index_6530,
           index_argument_against_a_type_parameter_named_range_is_admitted) {
    auto r = runAot(prog("    Console.WriteLine(\"ok\");\n"
                         "    return 0;\n",
                         "class Helper\n"
                         "  public static method F<Range>(d: Dictionary<Range, "
                         "int>): int\n"
                         "    return d[0];\n"
                         "  end F;\n"
                         "end Helper;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("ok\n"), r.stdoutText);
}

// The control for the index-argument check: a REAL key type (`Guid`,
// stdlib §2.6) is still rejected — §5.1 has no implicit conversion from a
// concrete primitive to a value type.
BEZEL_TEST(dict_range_index_6530,
           control_index_argument_against_a_real_key_type_is_still_guarded) {
    auto r = runAot(prog("    Console.WriteLine(\"ok\");\n"
                         "    return 0;\n",
                         "class Helper\n"
                         "  public static method F<T>(d: Dictionary<Guid, int>): "
                         "int\n"
                         "    return d[0];\n"
                         "  end F;\n"
                         "end Helper;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r,
        "the index argument to `Dictionary[key]` has a type that is not "
        "implicitly convertible to the target type"));
}

// And #6500's OWN repro shape — a string-keyed dictionary — must still be
// rejected. On the pre-#6500 compiler this reached codegen and produced
// malformed IR ("IR verification failed (malformed IR emitted)"); the guard
// turned it into a diagnostic, and the fix here must not turn it back into an
// accepted program.
BEZEL_TEST(dict_range_index_6530,
           control_string_keyed_dict_range_index_is_still_guarded_spec_23_4) {
    auto r = runAot(prog("    var d: Dictionary<string, string> := "
                         "[\"aa\": \"bb\"];\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

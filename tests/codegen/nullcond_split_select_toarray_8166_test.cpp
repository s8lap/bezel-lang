// Codegen / stdlib §5 — issue #8166: the `?.` CHAIN
// `s?.Split(",")?.Select(lambda(x) => x.Length)?.ToArray()` compiles and
// SEGVs in `bz_string_retain`.
//
// SPEC. §7.5 (v0.355, docs/Bezel_Language_Spec.md:1045-1057): "`x?.M`
// returns null if `x` is null; otherwise it performs the access. The result
// type is always nullable". The same section's evaluation paragraph (v0.355,
// :1057): "**The chain is the whole postfix sequence** (grammar
// [A.7](#sec-expressions)'s `PrimaryExpr`), its plain `.`, `[ ]` and `( )`
// suffixes included, and a suffix to the right of a `?.` is typed at the
// value it meets on the path where it runs — the preceding segment's own
// declared type, never the nullable type the `?.` gives the chain's result".
// stdlib §5 (v0.355, docs/Bezel_Standard_Library.md:3634) declares the LINQ
// surface; `Select<U>` (v0.355, :3658) is
// `public method Select<U>(selector: (T) -> U): IEnumerable<U>;` — the
// projection's RESULT type U is the sequence's ELEMENT type. stdlib §9.6
// (v0.355, :5368) declares `String`; `Split` (v0.355, :5447) is
// `public method Split(separator: string): owned Array<string>;`.
//
// THE DEFECT (MEASURED, ASan). The chain compiled and crashed at run time:
//
//   AddressSanitizer: SEGV on unknown address 0x000000000019
//     #0 bz_string_retain  src/runtime/bezel_runtime_refcounting.cpp:140
//     #1 bz_array_retain_strings src/runtime/bezel_runtime_collections.cpp:1000
//
// The chain's ELEMENT type was wrong, and the wrong type reached the RUNTIME
// as a string retain over int elements. The cause was in sema, not in the
// lowering: the `?.` spelling of a LINQ operator never typed its lambda
// argument's PARAMETER, because the helper that answers "what is the source
// element type T?" read only the `.` callee node (`ast::MemberAccessExpr`)
// and the `?.` spelling is a DISTINCT node (`ast::NullCondMemberExpr`,
// §7.5). So `x` in `lambda(x) => x.Length` had no type, the lambda's body
// resolved to nothing, and the lambda carried NO result type — and stdlib
// §5's `Select<U>` types its result element U from the selector's RESULT
// type, so sema fell back to the SOURCE element T. The chain therefore typed
// as `IEnumerable<string>` (not `IEnumerable<int>`) and the `?.ToArray()` on
// it as `Array<string>` (not `Array<int>`).
//
// Codegen lowers what sema says: it sized the lazy element slot as 8 bytes
// (a string) instead of 4 (an int) and emitted the bulk
// `bz_array_retain_strings` over the int elements — the retain read the
// int bits (`x.Length` = 1, 2, 2) as a string descriptor, and
// `bz_string_retain` faulted on the resulting `0x19` address.
//
// THE FIX. One change in the ONE helper that answers the receiver question
// (`linqLambdaElementType`, src/sema/ExprTypecheck_Calls.cpp): it now reads
// the `?.` callee node as well as the `.` one, so the `?.` spelling types
// its lambda argument's parameter at the receiver's own element type —
// exactly what §7.5's "a suffix to the right of a `?.` is typed at the value
// it meets on the path where it runs" requires. The result element type then
// flows `int` → `IEnumerable<int>` → `Array<int>`, and the lowering sizes
// the element slot as 4 bytes and emits no string retain.
//
// THE ORACLES. Every row asserts the PRINTED OUTPUT — the value, not just
// "it compiled" (a silent wrong value exits 0, so "it ran" is not an
// oracle). The element-type rows read the projected VALUES (1, 2, 2), which
// is what distinguishes an `Array<int>` from an `Array<string>`; the loop row
// is the issue's own program and must print `300`; the null-receiver row
// proves §7.5's short-circuit (the `??` fallback, not a crash); the
// shrunk-shape rows keep the neighbours of the defect honest.
//
// THE PROGRAM SHAPE. §7.5's chain is a chain of SUFFIXES, so the chain's
// segments are bound to locals here (`parts` / `lens` / `arr`) rather than
// written as one expression: the segments are the same `?.` suffixes over
// the same receivers, and the `?.` lowering under test — the `?.Select`
// segment's element type and the `?.ToArray` segment's drain — is the same
// code in both spellings. Binding them is what makes the programs
// leak-clean: an UNBOUND `Array<string>` temporary in a chain expression is
// not discharged with its elements, which is a PRE-EXISTING defect of the
// chain's disposal (the issue names it as a separate one, #7573) and is
// NOT what this fix is about. Row (B) keeps the issue's program verbatim —
// unbound temporaries and all — and runs on `runAot` for exactly that
// reason; under `BEZEL_AOT_FORCE_SANITIZE=address` it is still an ASan run,
// so the SEGV it used to crash with is still caught.

#include <string>
#include <utility>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program with one non-null nullable-string receiver (`s`) and one null
// receiver (`n`).
std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int "
           "    var s: string? := \"a,bc,dc\"; "
           "    var n: string? := null; " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// (A) The chain's VALUE — the element type is the oracle.
// ===========================================================================

// stdlib §5's `Select<U>`: the sequence's element type is the projection's
// result type U. `lambda(x) => x.Length` projects the three split parts
// "a", "bc", "dc" (lengths 1, 2, 2) to INTS, so the materialized array is
// `Array<int>` and the elements read back as 1, 2, 2.
//
// Before the fix the chain typed as `Array<string>`, so this row is the one
// that read the int bits as string descriptors (the SEGV) — and a build that
// "fixed" the crash by keeping the string element type would print the
// descriptor addresses instead of 1, 2, 2.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           stdlib_5_select_projects_the_element_type_through_the_nullcond_chain) {
    auto r = runAotChecked(prog(
        "    var parts := s?.Split(\",\"); "
        "    var lens := parts?.Select(lambda(x) => x.Length); "
        "    var arr := lens?.ToArray(); "
        "    Console.WriteLine(arr!.Count); "
        "    Console.WriteLine(arr![0]); "
        "    Console.WriteLine(arr![1]); "
        "    Console.WriteLine(arr![2]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n1\n2\n2\n");
}

// ===========================================================================
// (B) The issue's own program — 100 iterations, `300`.
// ===========================================================================

// The issue's repro verbatim: the loop accumulates `r!.Count` over 100
// iterations of the chain and must print `300` (3 elements × 100).
//
// `runAot`, not `runAotChecked`: this program's chain writes its segments
// into UNBOUND temporaries, and the chain's disposal of those temporaries
// is a pre-existing defect of its own (#7573), not of #8166 — so the leak
// oracle would fail the row for a reason this fix does not address. The
// row still asserts the exact stdout and that the program RAN: the SEGV it
// used to die with is a crash, and a crashed program does not run.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           issue_8166_the_100_iteration_loop_prints_300) {
    auto r = runAot(prog(
        "    var k := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var r := s?.Split(\",\")?.Select(lambda(x) => x.Length)?.ToArray(); "
        "      k := k + r!.Count; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(k); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "300\n");
}

// ===========================================================================
// (C) The 100x loop, with the chain's segments bound (leak-clean).
// ===========================================================================

// The same 100-iteration loop with the chain's segments bound to locals, so
// the program is leak-clean and can run under the sanitizer oracle.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           issue_8166_the_100_iteration_loop_prints_300_leak_clean) {
    auto r = runAotChecked(prog(
        "    var k := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var parts := s?.Split(\",\"); "
        "      var lens := parts?.Select(lambda(x) => x.Length); "
        "      var r := lens?.ToArray(); "
        "      k := k + r!.Count; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(k); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "300\n");
}

// ===========================================================================
// (D) §7.5's SHORT-CIRCUIT — a null receiver yields null, not a crash.
// ===========================================================================

// §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
// access." A null receiver must skip the WHOLE chain (§7.5's evaluation
// paragraph: "If any segment of a chain is null, the remainder of the chain
// is skipped on the same terms and the whole chain's result is `null`"), so
// the `??` fallback is what prints. The non-null twin (row A) proves the
// chain is not vacuous: it prints 3 there and the fallback here.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           spec_7_5_a_null_receiver_skips_the_whole_chain) {
    auto r = runAotChecked(prog(
        "    var parts := n?.Split(\",\"); "
        "    var lens := parts?.Select(lambda(x) => x.Length); "
        "    var arr := lens?.ToArray(); "
        "    Console.WriteLine(arr?.Count() ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// ===========================================================================
// (E) The SHRUNK SHAPES — the neighbours of the defect.
// ===========================================================================

// The shrink matrix measured while diagnosing #8166. The defect needs the
// `?.` ON THE PROJECTION (`?.Select`): with the projection spelled `.`, the
// chain's element type is right and the chain does not crash. Each shape is
// kept here so a future change cannot move the defect across a boundary.
//
//   a: `s?.Split(",")?.ToArray()`                  — 3 (no projection)
//   b: `s?.Split(",")!.Select(…).ToArray()`         — 3 (`?.` on Split only)
//   c: `s!.Split(",").Select(…)?.ToArray()`         — 3 (`?.` on ToArray only)
//   d: `s!.Split(",").Select(…).ToArray()`          — 3 (no `?.` at all)
//
// The `?.` ON THE PROJECTION is the shape that crashed.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           issue_8166_the_shrunk_shapes_keep_the_neighbours_honest) {
    auto r = runAotChecked(prog(
        "    var p1 := s?.Split(\",\"); "
        "    var a := p1?.ToArray(); "
        "    Console.WriteLine(a!.Count); "
        "    var p2 := s?.Split(\",\"); "
        "    var b := p2!.Select(lambda(x) => x.Length).ToArray(); "
        "    Console.WriteLine(b!.Count); "
        "    var p3 := s!.Split(\",\"); "
        "    var c := p3.Select(lambda(x) => x.Length)?.ToArray(); "
        "    Console.WriteLine(c!.Count); "
        "    var p4 := s!.Split(\",\"); "
        "    var d := p4.Select(lambda(x) => x.Length).ToArray(); "
        "    Console.WriteLine(d!.Count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n3\n3\n3\n");
}

// ===========================================================================
// (F) CONTROLS — the fix must not change the `.` spelling.
// ===========================================================================

// The `.` spelling is untouched by the fix: it is the row that already
// worked, and it keeps working.
BEZEL_TEST(codegen_nullcond_split_select_toarray_8166,
           stdlib_5_the_dot_spelling_is_untouched) {
    auto r = runAotChecked(prog(
        "    var p := s!.Split(\",\"); "
        "    var d := p.Select(lambda(x) => x.Length).ToArray(); "
        "    Console.WriteLine(d!.Count); "
        "    Console.WriteLine(d![0]); "
        "    Console.WriteLine(d![1]); "
        "    Console.WriteLine(d![2]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n1\n2\n2\n");
}

// Spec-derived regression test for issue #8121.
//
// SPEC §27.6 (v0.355, docs/Bezel_Language_Spec.md:6382-6450), *Method
// references* and *Method references to overloaded methods*, verbatim:
//
//   "For an extension method, the reference is formed in either of the two
//    spellings a call admits. **Type-qualified**, the reference uses the
//    extended type's name as qualifier, at the type arguments that bind a
//    generic block's parameters (§26.4); it captures no receiver, and the
//    resulting function has the receiver as its first parameter.
//    **Receiver-qualified** — `v.Scaled` over `extension Vec`, the member
//    resolved on the value exactly as the call `v.Scaled()` is (§26.5) — the
//    reference is a bound reference: the receiver is captured at formation …
//    and the resulting function has the method's own parameters only
//    (`var scaled: () -> int := v.Scaled;`)."
//
//   "A reference over a member of a **generic extension block** binds the
//    block's parameters by its qualifier (`IEnumerable<Pair>.KeepAll`) and the
//    member's own by a list after the name (`IEnumerable<int>.Map<string>`);
//    the qualifier's arguments instantiate the block member the same way
//    (§26.4)."
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the **target function type** at the use site. The
//    compiler selects the overload whose parameter types and return type unify
//    with the target's parameter and return types under the standard
//    implicit-conversion rules (§5.1)."
//
//   "When no target type is available (e.g., `var f := Math.Abs;` with no
//    annotation), the reference is ambiguous and the compiler reports an error
//    pointing to the candidate overloads. The user disambiguates by adding a
//    type annotation: `var f: (int) -> int := Math.Abs;`."
//
// SPEC §8.5 (v0.355), *ToString*, verbatim — the rendering of a function
// value (the (iv) ruling this file pins):
//
//   "For **function values** (§27.1): the stdlib §2.13 `FullName` rendering of
//    the value's own function type — the type it was **formed** at (§12.7
//    *Function-typed targets*, §27.6) — `"(int) -> int"`, `"() -> ()"` for a
//    procedure, parameter names not rendered; a lambda and a method reference
//    render alike …"
//
// THE DEFECTS (all measured on this fix's base, and all identical on a user
// class, on an extension block and on a built-in collection head — `Array`,
// `Queue`, `Stack`):
//
//   (i)  an OVERLOADED member is selected by the TARGET function type. With
//        `Len()` and `Len(x: int)` declared, `var g: (int) -> int := v.Len;`
//        was REFUSED ("the initializer for `g` has a type that is not
//        implicitly convertible to the target type (§5.1)") — the bare
//        reference carried the FIRST-DECLARED overload's signature and the
//        formation-site selection never got its say. Same for the
//        type-qualified spelling `var h: (V, int) -> int := V.Len;`.
//
//   (ii) a member of a GENERIC extension block, and the type-qualified
//        reference over a generic block (`extension Array<T>` +
//        `Array<int>.Head`), were refused the same way: the reference formed
//        over the block's ABSTRACT free variable `T` instead of the concrete
//        instantiation its qualifier binds (§26.4).
//
//   (iii) `var f := v.Len;` over TWO overloads and NO target type compiled
//        SILENTLY (the reference resolved by declaration order) where the
//        spec's no-target rule requires the ambiguity to be reported.
//
//   (iv) `Console.WriteLine(v.Len)` — a bare reference with no call —
//        compiles and prints `() -> int`. THIS IS CORRECT, and this file PINS
//        it: §27.6 admits the reference (a single candidate is not
//        ambiguous — the no-target rule above applies only to an OVERLOADED
//        name), and §8.5 renders a function value by the function type it was
//        formed at. So the printing is the specified behaviour, and the
//        (iv) row below is a CONTROL, not a defect.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE, never the exit code). The rejection rows assert the
// SPECIFIC diagnostic at a real source location. The CONTROL rows are
// load-bearing: the CALL spelling is unaffected, the user-class shape answers
// the same way as the extension shape (the gap was identical on both), and
// the no-target ambiguity is still reported for a user class.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §27.6 no-target ambiguity diagnostic (the spec's own rule, reported by
// name with the candidate overloads). The diagnostic names the MEMBER, so the
// needle is built per member name.
std::string noTargetAmbiguous(const std::string& name) {
    return "the method reference `" + name +
           "` is overloaded and has no target type to select by "
           "(\xC2\xA7""27.6 v0.307)";
}

const char kNoTargetAmbiguous[] =
    "the method reference `Len` is overloaded and has no target type to "
    "select by (\xC2\xA7""27.6 v0.307)";

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& src, const std::string& out) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The extension block of the (i) rows: TWO same-named members, so the
// reference is overloaded and the target function type is the only thing that
// can select between them (§27.6).
const char kExtOverloads[] =
    "extension V\n"
    "  public method Len(): int => 42;\n"
    "  public method Len(x: int): int => x + 100;\n"
    "end;\n";

// The user-class twin of the same shape — the gap was IDENTICAL there (the
// bare reference carried the first-declared overload's signature and the
// formation-site selection never ran).
const char kClassOverloads[] =
    "class V\n"
    "  public constructor() end constructor;\n"
    "  public method Len(): int => 42;\n"
    "  public method Len(x: int): int => x + 100;\n"
    "end V;\n";

// A generic extension block: the member's own signature mentions the block's
// free variable `T`, which the reference's qualifier binds (§26.4).
const char kGenericBlock[] =
    "extension Array<T>\n"
    "  public method Head(): T => this[0];\n"
    "end;\n";

// The (i)/(iii) programs over the extension block: a `V` value and the
// references formed over it.
std::string extProgram(const std::string& body) {
    return std::string("module M;\n"
                       "class V\n"
                       "  public constructor() end constructor;\n"
                       "end V;\n") +
           kExtOverloads +
           "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same over a USER CLASS — the parity shape.
std::string classProgram(const std::string& body) {
    return std::string("module M;\n") +
           kClassOverloads +
           "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The extension block over the BUILT-IN `Array` head (§26.5 rule 2 — the
// block extends the built-in collection type), with TWO same-named members, so
// the reference over it is overloaded and the target function type is the only
// thing that can select between them (§27.6).
const char kArrayExtOverloads[] =
    "extension Array<int>\n"
    "  public method Len2(): int => this.Count;\n"
    "  public method Len2(x: int): int => this.Count + x * 1000;\n"
    "end;\n";

// The (i)/(iii) programs over the built-in `Array` head.
std::string arrayProgram(const std::string& body) {
    return std::string("module M;\n") +
           kArrayExtOverloads +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ── (i) the OVERLOADED member is selected by the TARGET function type ───────

// §27.6 (v0.355): "When the named method (or property) is overloaded, the
// reference is resolved using the target function type at the use site." The
// target `(int) -> int` selects `Len(x: int)`; the target `() -> int` selects
// `Len()`. Both references are formed over the SAME name.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           extension_overload_is_selected_by_the_target_type) {
    // per §27.6 (v0.355) + §5.1 (v0.355)
    expectStdout(extProgram("var f: () -> int := v.Len;\n"
                             "    Console.WriteLine(f());\n"
                             "    var g: (int) -> int := v.Len;\n"
                             "    Console.WriteLine(g(5));"),
                 "42\n105\n");
}

// The TYPE-QUALIFIED spelling (§27.6: "the resulting function has the
// receiver as its first parameter"), over the same overload set: the target
// `(V, int) -> int` selects `Len(x: int)` and the receiver is the function's
// first parameter.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           type_qualified_extension_overload_is_selected_by_the_target_type) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355)
    expectStdout(extProgram("var h: (V, int) -> int := V.Len;\n"
                             "    Console.WriteLine(h(v, 6));"),
                 "106\n");
}

// The USER-CLASS twin of the first row: the gap was identical there, so the
// fix must answer there too (§27.6's rule is about the REFERENCE, not about
// extension blocks).
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           user_class_overload_is_selected_by_the_target_type) {
    // per §27.6 (v0.355) + §5.1 (v0.355)
    expectStdout(classProgram("var g: (int) -> int := v.Len;\n"
                               "    Console.WriteLine(g(5));"),
                 "105\n");
}

// ── (ii) a GENERIC extension block ──────────────────────────────────────────

// §26.4 / §27.6 (v0.355): "A reference over a member of a generic extension
// block binds the block's parameters by its qualifier" — so the reference
// over `extension Array<T>`'s `Head(): T` forms `() -> int` at an
// `Array<int>` receiver, and calling it runs the extension.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_member_reference_binds_the_block_parameters) {
    // per §26.4 (v0.355) + §27.6 (v0.355)
    expectStdout(std::string("module M;\n") + kGenericBlock +
                     "class C\n"
                     "  public static method Main(): int\n"
                     "    var a: Array<int> := [7,8,9];\n"
                     "    var f: () -> int := a.Head;\n"
                     "    Console.WriteLine(f());\n"
                     "    return 0;\n"
                     "  end Main;\n"
                     "end C;\n",
                 "7\n");
}

// The TYPE-QUALIFIED spelling over the same generic block (§27.6: "the
// reference uses the extended type's name as qualifier, at the type arguments
// that bind a generic block's parameters"): `Array<int>.Head` forms
// `(Array<int>) -> int` — the receiver as the first parameter.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           type_qualified_reference_over_a_generic_block_binds_them) {
    // per §26.4 (v0.355) + §27.6 (v0.355)
    expectStdout(std::string("module M;\n") + kGenericBlock +
                     "class C\n"
                     "  public static method Main(): int\n"
                     "    var a: Array<int> := [7,8,9];\n"
                     "    var g: (Array<int>) -> int := Array<int>.Head;\n"
                     "    Console.WriteLine(g(a));\n"
                     "    return 0;\n"
                     "  end Main;\n"
                     "end C;\n",
                 "7\n");
}

// ── (iii) NO target type and an OVERLOADED name: the reference is ambiguous ─

// §27.6 (v0.355): "When no target type is available (e.g., `var f :=
// Math.Abs;` with no annotation), the reference is ambiguous and the compiler
// reports an error pointing to the candidate overloads."
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           extension_overload_without_a_target_is_rejected_as_ambiguous) {
    // per §27.6 (v0.355)
    expectRejected(extProgram("var f := v.Len;"), kNoTargetAmbiguous);
}

// The USER-CLASS twin: the same rule, the same diagnostic — the extension
// shape must answer exactly like the class shape.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           user_class_overload_without_a_target_is_still_rejected) {
    // per §27.6 (v0.355)
    expectRejected(classProgram("var f := v.Len;"), kNoTargetAmbiguous);
}

// The same rule over a BUILT-IN COLLECTION head (§26.5 rule 2 — the block
// extends the built-in `Array` type): `var f := a.Len2;` over two same-named
// extension members with NO target type is the §27.6 no-target case and must
// be REJECTED, not resolved by declaration order.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           built_in_collection_overload_without_a_target_is_rejected_as_ambiguous) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355)
    expectRejected(arrayProgram("var f := a.Len2;"),
                   noTargetAmbiguous("Len2"));
}

// The same rule over the OTHER built-in collection heads — `Queue` and
// `Stack` are extended exactly as `Array` is (§26.5 rule 2), so the
// no-target rule must answer there too.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           queue_and_stack_overload_without_a_target_are_rejected_as_ambiguous) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355)
    expectRejected("module M;\n"
                   "extension Queue<int>\n"
                   "  public method Len2(): int => this.Count;\n"
                   "  public method Len2(x: int): int => this.Count + x * 1000;\n"
                   "end;\n"
                   "class C\n"
                   "  public static method Main(): int\n"
                   "    var q: Queue<int> := new Queue<int>();\n"
                   "    var f := q.Len2;\n"
                   "    return 0;\n"
                   "  end Main;\n"
                   "end C;\n",
                   noTargetAmbiguous("Len2"));
    expectRejected("module M;\n"
                   "extension Stack<int>\n"
                   "  public method Len2(): int => this.Count;\n"
                   "  public method Len2(x: int): int => this.Count + x * 1000;\n"
                   "end;\n"
                   "class C\n"
                   "  public static method Main(): int\n"
                   "    var s: Stack<int> := new Stack<int>();\n"
                   "    var f := s.Len2;\n"
                   "    return 0;\n"
                   "  end Main;\n"
                   "end C;\n",
                   noTargetAmbiguous("Len2"));
}

// ── (i) over a BUILT-IN COLLECTION head: the TARGET selects the overload ───

// §27.6 (v0.355): "When the named method (or property) is overloaded, the
// reference is resolved using the target function type at the use site." Over
// the built-in `Array` head: the target `(int) -> int` selects `Len2(x: int)`
// (3 + 5 * 1000) and the target `() -> int` selects `Len2()` (3) — the SAME
// name, two targets, two members.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           built_in_collection_overload_is_selected_by_the_target_type) {
    // per §27.6 (v0.355) + §5.1 (v0.355) + §26.5 rule 2 (v0.355)
    expectStdout(arrayProgram("var g: (int) -> int := a.Len2;\n"
                              "    Console.WriteLine(g(5));\n"
                              "    var h: () -> int := a.Len2;\n"
                              "    Console.WriteLine(h());"),
                 "5003\n3\n");
}

// The same generic block over a NON-primitive element type: the block's free
// variable `T` binds to `string` at the `Array<string>` receiver, so the
// reference forms `() -> string` (§26.4) and prints the head element.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_member_reference_binds_the_block_parameters_over_a_string_array) {
    // per §26.4 (v0.355) + §27.6 (v0.355)
    expectStdout(std::string("module M;\n") + kGenericBlock +
                     "class C\n"
                     "  public static method Main(): int\n"
                     "    var a: Array<string> := [\"hi\",\"there\"];\n"
                     "    var f: () -> string := a.Head;\n"
                     "    Console.WriteLine(f());\n"
                     "    return 0;\n"
                     "  end Main;\n"
                     "end C;\n",
                 "hi\n");
}

// ── (iv) the RULING: a bare reference with no target PRINTS its type ───────

// §27.6 admits the reference (a SINGLE candidate is not ambiguous — the
// no-target rule above applies only to an OVERLOADED name), and §8.5 renders
// a function value by the function type it was FORMED at: `() -> int`.
// This row pins that rendering, so a later change that "fixes" the printing
// by rejecting the reference would break this row.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           a_bare_reference_without_a_target_prints_its_function_type) {
    // per §8.5 (v0.355) + §27.6 (v0.355)
    expectStdout("module M;\n"
                 "class V\n"
                 "  public constructor() end constructor;\n"
                 "end V;\n"
                 "extension V\n"
                 "  public method Len(): int => 42;\n"
                 "end;\n"
                 "class C\n"
                 "  public static method Main(): int\n"
                 "    var v := new V();\n"
                 "    Console.WriteLine(v.Len);\n"
                 "    return 0;\n"
                 "  end Main;\n"
                 "end C;\n",
                 "() -> int\n");
}

// ── CONTROLS ────────────────────────────────────────────────────────────────

// The CALL spelling — the arm every reference must agree with (§27.6: the
// receiver-qualified reference resolves "the member … exactly as the call
// `v.Scaled()` is"). Unaffected by the fix.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           extension_member_call_still_answers) {
    // per §27.6 (v0.355)
    expectStdout(extProgram("Console.WriteLine(v.Len());"), "42\n");
}

// A SINGLE extension member with no target type is NOT ambiguous: the
// reference forms at the callee's own signature and the program runs. This is
// the control for the (iii) rows — the no-target rule is about an OVERLOADED
// name, not about every reference without a target.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           a_single_extension_member_without_a_target_is_not_ambiguous) {
    // per §27.6 (v0.355)
    expectStdout("module M;\n"
                 "class V\n"
                 "  public constructor() end constructor;\n"
                 "end V;\n"
                 "extension V\n"
                 "  public method Len(): int => 42;\n"
                 "end;\n"
                 "class C\n"
                 "  public static method Main(): int\n"
                 "    var v := new V();\n"
                 "    var f := v.Len;\n"
                 "    Console.WriteLine(f());\n"
                 "    return 0;\n"
                 "  end Main;\n"
                 "end C;\n",
                 "42\n");
}

// The same control over a BUILT-IN collection head: a single extension member
// with no target type is not ambiguous there either — the no-target rule
// reaches the OVERLOADED name only.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           a_single_built_in_collection_extension_member_without_a_target_is_not_ambiguous) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355)
    expectStdout("module M;\n"
                 "extension Array<int>\n"
                 "  public method Doubled(): int => this.Count * 2;\n"
                 "end;\n"
                 "class C\n"
                 "  public static method Main(): int\n"
                 "    var a: Array<int> := [1,2,3];\n"
                 "    var f := a.Doubled;\n"
                 "    Console.WriteLine(f());\n"
                 "    return 0;\n"
                 "  end Main;\n"
                 "end C;\n",
                 "6\n");
}

// The CALL spelling over the SAME overloaded extension members: the no-target
// rule of §27.6 is about a REFERENCE with no target type — a call has its own
// argument types and resolves by them (§22.6), so both calls still answer.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           the_call_spelling_over_the_same_overloads_still_resolves) {
    // per §22.6 (v0.355) + §27.6 (v0.355)
    expectStdout(arrayProgram("Console.WriteLine(a.Len2());\n"
                              "    Console.WriteLine(a.Len2(5));"),
                 "3\n5003\n");
}

// ── fix-forward (review r39-rev-8121): the AMBIGUITY diagnostic's candidates ─
//
// §27.6 (v0.355), *Selection among several unifying candidates*: "candidates
// that unify only through conversion are co-equal ..., so absent an exact
// candidate the reference is ambiguous and the compiler reports the
// candidates." A candidate is an overload of the TARGET's arity: with
// `M()`, `M(x: long)` and `M(x: double)` against `(int) -> int`, the two
// one-parameter overloads both unify through a widening conversion and
// neither is exact, so the reference is ambiguous between exactly those two.
//
// Measured on 2bf3d627 (the fix this row repairs): the compiler itself
// crashed (SIGSEGV in `checkAssignable`) — the candidate loop of the
// ambiguity diagnostic had lost its arity filter and indexed the
// zero-parameter `M()`'s parameter list by the target's arity. With an
// extra `M(x: long, y: int)` it listed that two-parameter overload as a
// candidate. Both on a user class and on an extension block.

namespace {

const char kAmbiguousTwoCandidates[] =
    "the method reference `M` is ambiguous against the target type "
    "(\xC2\xA7""27.6 v0.307) \xE2\x80\x94 multiple overloads unify through "
    "implicit conversion and none matches exactly (candidates: "
    "`(long) -> int`, `(double) -> int`)";

// `decls` declares the overloads: in the class body (`inClass`) or in an
// extension block over the class.
std::string ambiguityProgram(const std::string& decls, bool inClass) {
    std::string src = "module M;\nclass V\n  public constructor() end constructor;\n";
    if (inClass) src += decls;
    src += "end V;\n";
    if (!inClass) src += "extension V\n" + decls + "end;\n";
    src += "class C\n"
           "  public static method Main(): int\n"
           "    var v := new V();\n"
           "    var f: (int) -> int := v.M;\n"
           "    Console.WriteLine(f(1));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
    return src;
}

const char kFewerParamOverloads[] =
    "  public method M(): int => 1;\n"
    "  public method M(x: long): int => 2;\n"
    "  public method M(x: double): int => 3;\n";

const char kMoreParamOverloads[] =
    "  public method M(x: long): int => 2;\n"
    "  public method M(x: double): int => 3;\n"
    "  public method M(x: long, y: int): int => 4;\n";

}  // namespace

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           class_ambiguity_with_a_fewer_parameter_overload_reports_the_two_candidates) {
    // per §27.6 (v0.355)
    expectRejected(ambiguityProgram(kFewerParamOverloads, true),
                   kAmbiguousTwoCandidates);
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           extension_ambiguity_with_a_fewer_parameter_overload_reports_the_two_candidates) {
    // per §27.6 (v0.355) + §26.5 (v0.355)
    expectRejected(ambiguityProgram(kFewerParamOverloads, false),
                   kAmbiguousTwoCandidates);
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           class_ambiguity_does_not_list_a_wrong_arity_overload) {
    // per §27.6 (v0.355)
    expectRejected(ambiguityProgram(kMoreParamOverloads, true),
                   kAmbiguousTwoCandidates);
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           extension_ambiguity_does_not_list_a_wrong_arity_overload) {
    // per §27.6 (v0.355) + §26.5 (v0.355)
    expectRejected(ambiguityProgram(kMoreParamOverloads, false),
                   kAmbiguousTwoCandidates);
}

// ── fix-forward (review r39-rev-8121): a GENERIC-BLOCK reference RUNS the ───
// ── instantiated member ────────────────────────────────────────────────────
//
// §27.6 (v0.355): "A reference over a member of a generic extension block
// binds the block's parameters by its qualifier ...; the qualifier's
// arguments instantiate the block member the same way (§26.4)." So the
// function value formed by `b.Head` (bound) or `Array<long>.Head` (type-
// qualified) over `extension Array<T>`'s `Head(): T` returns what the call
// `b.Head()` returns.
//
// Measured on 2bf3d627: sema formed `() -> long`, but the thunk called the
// block's i32-placeholder template — `5000000000` came back as 705032704, a
// `double` 2.5 as 0, and a heap string / class element as a truncated pointer
// (a SEGV under ASan). The int and string-LITERAL rows above could not see it.
// These rows run under the checked (ASan) oracle, in both spellings, and read
// the data the thunk handles (a value above 2^32, a heap-allocated string).

namespace {

// The BOUND reference is formed and called in `Main`, over a local; the
// TYPE-QUALIFIED reference is formed and called in `Show`, over the same
// value passed as a parameter. (Kept apart on purpose: a type-qualified
// reference called with a LOCAL collection as its receiver argument leaks
// that collection on some compiles — measured on the #8120 base 824ad6b5
// with a NON-generic `extension Array<int>` too, so it is not this fix's —
// and a checked row must not depend on it.)
std::string genericBlockProgram(const std::string& block,
                                const std::string& recvType,
                                const std::string& init,
                                const std::string& bound,
                                const std::string& qualified) {
    return std::string("module M;\n"
                       "class Item\n"
                       "  public n: int;\n"
                       "  public constructor(n: int)\n"
                       "    this.n := n;\n"
                       "  end constructor;\n"
                       "end Item;\n") +
           block +
           "class C\n"
           "  public static method Show(b: " + recvType + "): int\n" +
           qualified +
           "    return 0;\n"
           "  end Show;\n"
           "  public static method Main(): int\n" + init + bound +
           "    return C.Show(b);\n"
           "  end Main;\n"
           "end C;\n";
}

void expectCheckedStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_over_a_long_array_returns_the_full_value) {
    // per §27.6 (v0.355) + §26.4 (v0.355)
    expectCheckedStdout(
        genericBlockProgram(kGenericBlock, "Array<long>",
                            "    var b: Array<long> := [5000000000L, 2L];\n",
                            "    Console.WriteLine(b.Head());\n"
                            "    var g: () -> long := b.Head;\n"
                            "    Console.WriteLine(g());\n",
                            "    var h: (Array<long>) -> long := Array<long>.Head;\n"
                            "    Console.WriteLine(h(b));\n"),
        "5000000000\n5000000000\n5000000000\n");
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_over_a_double_array_returns_the_value) {
    // per §27.6 (v0.355) + §26.4 (v0.355)
    expectCheckedStdout(
        genericBlockProgram(kGenericBlock, "Array<double>",
                            "    var b: Array<double> := [2.5, 1.0];\n",
                            "    var k: () -> double := b.Head;\n"
                            "    Console.WriteLine(k());\n",
                            "    var m: (Array<double>) -> double := Array<double>.Head;\n"
                            "    Console.WriteLine(m(b));\n"),
        "2.5\n2.5\n");
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_over_a_heap_string_array_returns_the_string) {
    // per §27.6 (v0.355) + §26.4 (v0.355)
    expectCheckedStdout(
        genericBlockProgram(kGenericBlock, "Array<string>",
                            // The element is built in place: a heap string
                            // bound to a local first and then copied into the
                            // literal leaks 32 bytes with NO reference at all
                            // (pre-existing on 824a and 2bf3d627).
                            "    var n := 41;\n"
                            "    var b: Array<string> := [\"v\" + n.ToString(), \"there\"];\n",
                            // Each spelling is called TWICE: a +0 result the
                            // closure caller releases frees the element on the
                            // first call, so the second reads freed memory.
                            "    var f: () -> string := b.Head;\n"
                            "    Console.WriteLine(f());\n"
                            "    Console.WriteLine(f());\n",
                            "    var g: (Array<string>) -> string := Array<string>.Head;\n"
                            "    Console.WriteLine(g(b));\n"
                            "    Console.WriteLine(g(b));\n"),
        "v41\nv41\nv41\nv41\n");
}

BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_over_an_owning_array_returns_the_element) {
    // per §27.6 (v0.355) + §26.4 (v0.355)
    expectCheckedStdout(
        genericBlockProgram("extension OwningArray<T> where T: class\n"
                            "  public method Head(): T => this[0];\n"
                            "end;\n",
                            "OwningArray<Item>",
                            "    using var b: OwningArray<Item> := new OwningArray<Item>();\n"
                            "    b.Add(new Item(5));\n",
                            "    var f: () -> Item := b.Head;\n"
                            "    Console.WriteLine(f().n);\n",
                            "    var g: (OwningArray<Item>) -> Item := OwningArray<Item>.Head;\n"
                            "    Console.WriteLine(g(b).n);\n"),
        "5\n5\n");
}

// A block member whose PARAMETER is the block's variable: the thunk's
// parameter is the instantiated `long` too, so the argument is not truncated.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_with_a_type_variable_parameter_passes_the_full_value) {
    // per §27.6 (v0.355) + §26.4 (v0.355)
    expectCheckedStdout(
        genericBlockProgram("extension Array<T>\n"
                            "  public method Same(x: T): T => x;\n"
                            "end;\n",
                            "Array<long>",
                            "    var b: Array<long> := [1L];\n",
                            "    var f: (long) -> long := b.Same;\n"
                            "    Console.WriteLine(f(6000000000L));\n",
                            "    var g: (Array<long>, long) -> long := Array<long>.Same;\n"
                            "    Console.WriteLine(g(b, 7000000000L));\n"),
        "6000000000\n7000000000\n");
}

// The function value's call hands back an OWNED `string` (§28.9): a closure
// call result is released by its caller, as a lambda body's is (#429/#2688).
// The inlined generic member's `this[0]` is a BORROW of the element, so the
// thunk mints the +1; a BLOCK body's `return this[0];` against a declared `T`
// does not mint either. Measured before the mint: `bezel: fatal: refcount
// over-release (string)` on the second call, both spellings, both bodies.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_over_a_heap_string_array_block_body_returns_the_string) {
    // per §27.6 (v0.355) + §26.4 (v0.355) + §28.9 (v0.355)
    expectCheckedStdout(
        genericBlockProgram("extension Array<T>\n"
                            "  public method Head(): T\n"
                            "    return this[0];\n"
                            "  end Head;\n"
                            "end;\n",
                            "Array<string>",
                            "    var n := 41;\n"
                            "    var b: Array<string> := [\"v\" + n.ToString(), \"there\"];\n",
                            "    var f: () -> string := b.Head;\n"
                            "    Console.WriteLine(f());\n"
                            "    Console.WriteLine(f());\n",
                            "    var g: (Array<string>) -> string := Array<string>.Head;\n"
                            "    Console.WriteLine(g(b));\n"
                            "    Console.WriteLine(g(b));\n"),
        "v41\nv41\nv41\nv41\n");
}

// The mint's TRANSFER side: a member that already returns +1 (a `+`
// producer, in either body form) or a literal sentinel is not retained again
// — a surplus +1 would leak one string per call, so the calls are looped.
BEZEL_TEST(sema_extension_method_ref_residual_8121,
           generic_block_reference_to_a_string_producer_member_does_not_leak) {
    // per §27.6 (v0.355) + §26.4 (v0.355) + §28.9 (v0.355)
    expectCheckedStdout(
        genericBlockProgram("extension Array<T>\n"
                            "  public method Desc(): string => \"n=\" + this.Count.ToString();\n"
                            "  public method Lit(): string => \"lit\";\n"
                            "  public method DescB(): string\n"
                            "    return \"m=\" + this.Count.ToString();\n"
                            "  end DescB;\n"
                            "end;\n",
                            "Array<string>",
                            "    var n := 41;\n"
                            "    var b: Array<string> := [\"v\" + n.ToString(), \"there\"];\n"
                            "    var i := 0;\n",
                            "    while i < 50 do\n"
                            "      var h: () -> string := b.Lit;\n"
                            "      var k: () -> string := b.DescB;\n"
                            "      var t := k();\n"
                            "      if i = 49 then\n"
                            "        Console.WriteLine(h());\n"
                            "        Console.WriteLine(t);\n"
                            "      end;\n"
                            "      i := i + 1;\n"
                            "    end;\n",
                            "    var j := 0;\n"
                            "    while j < 50 do\n"
                            "      var g: (Array<string>) -> string := Array<string>.Desc;\n"
                            "      var s := g(b);\n"
                            "      if j = 49 then\n"
                            "        Console.WriteLine(s);\n"
                            "      end;\n"
                            "      j := j + 1;\n"
                            "    end;\n"),
        "lit\nm=2\nn=2\n");
}

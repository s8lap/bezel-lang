// Issue #7164 (spec §14.1, v0.343; Appendix C.1) -- a generic type
// reference in an EXPRESSION position must supply EXACTLY the declared number
// of type arguments, under the TYPE position's own arity rule.
//
// THE GOVERNING TEXT. spec §14.1 (v0.343): "Every such reference supplies
// exactly the declared number of type arguments. Bezel has no raw and no open
// generic type reference: `Box` bare and `Box<int, string>` are equally
// compile errors at a `class Box<T>`, each naming the declared arity, and the
// same holds for a generic type alias. ... The rule holds inside a generic
// type's own declaration -- its member bodies, field initializers and member
// signatures: there, too, the bare `Box` fixes arity 0." Appendix C.1 lists
// the arity rule among the compiler-enforced invariants.
//
// THE DEFECT. The compiler enforced §14.1 in the TYPE position only. An
// EXPRESSION-position type reference -- `var v := Box<int>;`,
// `Box<int>.Make(...)` -- is an `ast::IdentifierExpr` carrying type
// arguments, not an `ast::Type` node, so the type position's arity check was
// never consulted for it. A wrong count was therefore ACCEPTED SILENTLY and
// the program RAN (the type position and the expression position disagreed
// about the SAME reference): `Box<int>` at a `class Box` (zero parameters)
// degraded to a zero-initialized value behind the internal #2434 warning, and
// `Tree<int>.Node.Make()` at a `class Node<U>` (one parameter, ZERO supplied)
// printed `5` and exited 0.
//
// THE ISSUE'S OWN REPROS. The three rows below are the issue's own table:
// `var v := Box<int>;` at a zero-parameter `Box`, `Box<int>.Make()` at the
// same `Box`, and `Tree<int>.Node.Make()` at a nested `Node<U>`; plus the
// mirror shape the issue names, `Box<int, string>` at a `class Box<T>`.
//
// THE FIX LANDED ELSEWHERE (before this test existed). The defect was fixed
// by commit 88247761e ("narrow #7164: enforce §14.1's arity rule at the two
// expression positions", 2026-10-25): "Two distinct wrongs, both in
// expression position, both now rejected with the type position's own text:
// a wrong count on a type reference ...; and a missing count on a nested
// type". Nothing closed the issue, so it stayed open on a tree that already
// behaves correctly.
//
// ORACLE. `runAot` (the AOT path). The reject rows assert the SPECIFIC
// diagnostic text -- "generic-argument count does not match type's
// generic-parameter count" -- AND that the refusal is LOCATED (the house's
// own `failedAtCompileTimeWithRealLocation`), and that the program did NOT
// run. The control row asserts the EXACT STDOUT of the correctly-arity'd
// references, so the rule cannot be satisfied by refusing every generic
// reference.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

// The program is built around the caller's body; `decls` carries the class
// declarations the body needs.
std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A zero-parameter `Box` -- `Box<int>` supplies one argument to a type that
// declares none.
const std::string kBoxZero =
    "public class Box\n"
    "  public static method Make(): int\n"
    "    return 5;\n"
    "  end Make;\n"
    "end Box;\n";

// A one-parameter `Box<T>` -- `Box<int>` is correct here and `Box<int,
// string>` is not.
const std::string kBoxOne =
    "public class Box<T>\n"
    "  public static method Make(): int\n"
    "    return 5;\n"
    "  end Make;\n"
    "end Box;\n";

// A one-parameter `Tree<T>` with a nested one-parameter `Node<U>`.
const std::string kTree =
    "public class Tree<T>\n"
    "  public class Node<U>\n"
    "    public static method Make(): int\n"
    "      return 7;\n"
    "    end Make;\n"
    "  end Node;\n"
    "end Tree;\n";

// The row is a REJECTION: the program must not run, the front end must have
// reported an error AT A REAL LOCATION, and the error must carry the type
// position's own arity text.
void expectArityRejected(const std::string& decls, const std::string& body) {
    JitResult r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The row claims a LOCATED error, so the row ASSERTS the location: the
    // house's own located-rejection helper
    // (tests/support/aot_result_asserts.hpp) requires a `file:line:column:`
    // prefix on a diagnostic that is not a crash, a hang or a sanitizer
    // report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type's generic-parameter "
           "count"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& expected) {
    JitResult r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// =====================================================================
// ROW 1 -- the issue's own first row: `var v := Box<int>;` where `Box`
// declares ZERO type parameters. §14.1 (v0.343) makes every such reference
// supply exactly the declared number of type arguments, and the TYPE
// position already said so.
//
// Pre-fix: ACCEPTED -- the reference degraded to a zero-initialized value
//           behind the internal #2434 warning, and the program exited 0.
// Post-fix: REJECTED with the type position's own arity text.
// =====================================================================
BEZEL_TEST(generic_reference_arity_expression_position_7164,
           a_wrong_type_argument_count_at_a_type_reference_is_refused_14_1) {
    expectArityRejected(kBoxZero,
                        "    var v := Box<int>;\n"
                        "    Console.WriteLine(\"compiled\");\n");
}

// =====================================================================
// ROW 2 -- the same wrong count at the CALL spelling: `Box<int>.Make()`.
// §14.1: "`Box` bare and `Box<int, string>` are equally compile errors at a
// `class Box<T>`, each naming the declared arity" -- the expression position
// must name it too.
//
// Pre-fix: ACCEPTED -- the program printed `5` and exited 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(generic_reference_arity_expression_position_7164,
           a_wrong_type_argument_count_at_a_static_call_is_refused_14_1) {
    expectArityRejected(kBoxZero,
                        "    Console.WriteLine(Box<int>.Make());\n");
}

// =====================================================================
// ROW 3 -- the issue's second distinct wrong: a MISSING count on a nested
// type. `Tree<int>.Node.Make()` where `Node<U>` declares one parameter and
// the reference supplies ZERO (§14.1: "the bare `Box` fixes arity 0" --
// here the bare `Node` fixes arity 1).
//
// Pre-fix: ACCEPTED -- the program printed `5` and exited 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(generic_reference_arity_expression_position_7164,
           a_missing_type_argument_count_on_a_nested_type_is_refused_14_1) {
    expectArityRejected(kTree,
                        "    Console.WriteLine(Tree<int>.Node.Make());\n");
}

// =====================================================================
// ROW 4 -- the mirror shape the issue names: `Box<int, string>` at a
// `class Box<T>`, which declares exactly ONE type parameter.
//
// Pre-fix: ACCEPTED -- the reference degraded to a zero-initialized value
//           behind the internal #2434 warning, and the program exited 0.
// Post-fix: REJECTED.
// =====================================================================
BEZEL_TEST(generic_reference_arity_expression_position_7164,
           a_too_large_type_argument_list_is_refused_14_1) {
    expectArityRejected(kBoxOne,
                        "    var v := Box<int, string>;\n"
                        "    Console.WriteLine(\"compiled\");\n");
}

// =====================================================================
// CONTROL -- the CORRECTLY-arity'd references still work and still print
// the right value. A VALUE oracle (exact stdout), green on both arms: it
// pins that §14.1's rule refuses only the WRONG counts, and that the
// nested-type spelling with both lists supplied still reaches the callee.
//
// Pre-fix and post-fix: "5" and "7".
// =====================================================================
BEZEL_TEST(generic_reference_arity_expression_position_7164,
           the_correctly_arity_d_references_still_work_14_1) {
    expectStdout(kBoxOne + kTree,
                 "    Console.WriteLine(Box<int>.Make());\n"
                 "    Console.WriteLine(Tree<int>.Node<int>.Make());\n",
                 "5\n7");
}

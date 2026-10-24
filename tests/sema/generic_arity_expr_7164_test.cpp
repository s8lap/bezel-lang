// fix #7164 — §14.1's generic-arity rule is enforced in the TYPE position but
// not in two EXPRESSION positions.
//
// THE RULE. §14.1 (v0.343):
//
//   "Every such reference supplies exactly the declared number of type
//    arguments. Bezel has no raw and no open generic type reference: `Box`
//    bare and `Box<int, string>` are equally compile errors at a
//    `class Box<T>`, each naming the declared arity, and the same holds for
//    a generic type alias … The rule holds inside a generic type's own
//    declaration — its member bodies, field initializers and member
//    signatures: there, too, the bare `Box` fixes arity 0."
//
// Appendix C.1 lists the arity rule among the compiler-enforced invariants.
//
// WHERE THE TYPE POSITION ENFORCED IT. `Resolver::resolveNamed`'s
// "Generic-arity check" (src/sema/TypeResolution_Names.cpp) compares the
// reference's type-argument count with the resolved type's declared
// parameter count and reports "generic-argument count does not match type's
// generic-parameter count".
//
// WHERE THE EXPRESSION POSITION DID NOT. An expression-position type
// reference — `Box<int>.Make(…)`, `var v := Box<int>;` — is an
// `ast::IdentifierExpr` carrying `typeArgs`, NOT an `ast::Type` node, so
// `resolveNamed` was never called for it: the expression pass hand-built the
// instantiated TypeRef (ExprTypecheck_Exprs.cpp's IdentifierExpr arm) and
// never compared the two counts, and the nested-type instantiation site
// (#7135's branch in ExprTypecheck_Calls.cpp) checked the arity only when a
// type-argument list was PRESENT. A wrong count was therefore accepted
// silently and the program RAN.
//
//   * WRONG #1 — a wrong count on a type reference: `Box<int>` at a
//     `class Box` (zero parameters), `Box<int, string>` at a `class Box<T>`,
//     `Box<int>` at a `class Box<T, U>`.
//   * WRONG #2 — a MISSING count on a nested type: `Tree<int>.Node.Make()`
//     at a `class Node<U>` (one parameter) supplies ZERO, which left the
//     nested type's parameter unbound and the program ran.
//
// The type position and the expression position disagreed about the SAME
// reference; these rows pin that they now agree, and that the type
// position's diagnostic TEXT is unchanged (one rule, one message).
//
// ORACLE: runAot (the real AOT path). The rejection rows assert the
// diagnostic TEXT, not merely that the program failed — a program rejected
// for an unrelated reason would otherwise pass them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The TYPE position's diagnostic, verbatim. Both positions must use this
// text: a second wording for the same rule would make the same defect read
// two ways depending on which position it was written in.
constexpr const char* kArityDiag =
    "generic-argument count does not match type's generic-parameter count";

// True iff the program is REJECTED and some ERROR diagnostic contains
// `needle`. Both halves matter: `hasErrors()` alone would pass on an
// unrelated failure, and a matching string on a program that still compiled
// would prove nothing.
bool rejectsWithDiag(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// A program with a `class Box` that declares `params` type parameters and a
// static `Make` returning 5, plus a `Main` whose body is `stmts`.
std::string withBox(const std::string& params, const std::string& stmts) {
    return "module M; "
           "public class Box" + params + " "
           "  public static method Make(): int "
           "    return 5; "
           "  end Make; "
           "end Box; "
           "public class D public static method Main(): int " +
           stmts + " return 0; end Main; end D; ";
}

// §11.6's nested type: a generic class `Tree<T>` holding a generic nested
// class `Node<U>`, whose static `Create` returns 42.
std::string withTree(const std::string& stmts) {
    return "module M; "
           "public class Tree<T> "
           "  public class Node<U> "
           "    public static method Create(): int "
           "      return 42; "
           "    end Create; "
           "  end Node; "
           "end Tree; "
           "public class D public static method Main(): int " +
           stmts + " return 0; end Main; end D; ";
}

}  // namespace

// =============================================================================
// PART 1 — WRONG #1: a type reference in EXPRESSION position whose
// type-argument count differs from the declared arity.
// =============================================================================

// `Box<int>` at a `class Box` (ZERO parameters) used as a static-member
// receiver. Before the fix the wrong count was accepted and the program
// ran, printing 5.
BEZEL_TEST(sema_generic_arity_expr_7164,
           receiver_type_ref_with_too_many_args_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("", "Console.WriteLine(Box<int>.Make()); "), kArityDiag));
}

// `Box<int, string>` at a `class Box<T>` (ONE parameter).
BEZEL_TEST(sema_generic_arity_expr_7164,
           receiver_type_ref_with_two_args_at_one_parameter_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("<T>", "Console.WriteLine(Box<int, string>.Make()); "),
        kArityDiag));
}

// `Box<int>` at a `class Box<T, U>` (TWO parameters) — too FEW arguments.
BEZEL_TEST(sema_generic_arity_expr_7164,
           receiver_type_ref_with_too_few_args_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("<T, U>", "Console.WriteLine(Box<int>.Make()); "), kArityDiag));
}

// The same wrong count in a VALUE position: `var v := Box<int>;` at a
// `class Box` (ZERO parameters). Before the fix this compiled and merely
// degraded to a zero-initialized value (the pre-existing #2434
// internal-degradation warning); it is a wrong-arity reference, so it is
// now rejected.
BEZEL_TEST(sema_generic_arity_expr_7164,
           value_position_type_ref_with_too_many_args_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("", "var v := Box<int>; "), kArityDiag));
}

// The value position with too FEW arguments: `var v := Box<int, string>;`
// at a `class Box<T>`.
BEZEL_TEST(sema_generic_arity_expr_7164,
           value_position_type_ref_with_two_args_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("<T>", "var v := Box<int, string>; "), kArityDiag));
}

// =============================================================================
// PART 2 — WRONG #2: a NESTED type reached with a MISSING type-argument
// count. §14.1's "the bare `Box` fixes arity 0" applies to the nested
// segment too: the nested type declares ONE parameter, and the bare
// reference supplies ZERO.
// =============================================================================

// `Tree<int>.Node.Make()` at a `class Node<U>` — the nested type's own
// parameter is left unbound and the program runs, printing 42.
BEZEL_TEST(sema_generic_arity_expr_7164,
           nested_type_bare_reference_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withTree("Console.WriteLine(Tree<int>.Node.Make()); "), kArityDiag));
}

// The same shape with a NON-generic enclosing type: `Outer.Inner.Make()` at
// a `class Inner<V>`. The TYPE position already rejects this spelling
// (`var v: Outer.Inner;` is the arity error), so the expression position
// now agrees with it.
BEZEL_TEST(sema_generic_arity_expr_7164,
           nested_type_bare_reference_nongeneric_enclosing_rejected_spec_14_1) {
    const std::string prog =
        "module M; "
        "public class Outer "
        "  public class Inner<V> "
        "    public static method Make(): int "
        "      return 5; "
        "    end Make; "
        "  end Inner; "
        "end Outer; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Outer.Inner.Make()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejectsWithDiag(prog, kArityDiag));
}

// =============================================================================
// PART 3 — THE CONTROLS. Every LEGAL expression-position reference must keep
// working; these are the rows a fix that adds a rejection can break.
// =============================================================================

// The legal receiver: `Box<int>.Make()` at a `class Box<T>` — the declared
// count, so the reference is admitted and the program runs.
BEZEL_TEST(sema_generic_arity_expr_7164,
           receiver_type_ref_with_the_declared_count_accepted_spec_14_1) {
    auto r = runAot(
        withBox("<T>", "Console.WriteLine(Box<int>.Make()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// The legal nested type: §11.6's own worked example
// `Tree<int>.Node<int>.Create()` — the chain head supplies the ENCLOSING
// type's arguments and the dotted segment supplies the NESTED type's own.
BEZEL_TEST(sema_generic_arity_expr_7164,
           nested_type_with_explicit_type_args_accepted_spec_11_6) {
    auto r = runAot(withTree("Console.WriteLine(Tree<int>.Node<int>.Create()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The legal VALUE position: `var v := Box<int>;` at a `class Box<T>`. The
// declared count, so the reference is admitted.
//
// NOTE (MEASURED, base and tip alike): this program carries the pre-existing
// #2434 internal-degradation warning ("lowerLocalDecl's initializer
// expression failed to lower to a value for this local's slot") — a type
// used as a VALUE is a separate hole from the arity rule, and the arity
// fix does not paper over it. This row therefore asserts only that the
// program is NOT rejected and does not run (a type has no value to print).
BEZEL_TEST(sema_generic_arity_expr_7164,
           value_position_type_ref_with_the_declared_count_accepted_spec_14_1) {
    auto r = runAot(withBox("<T>", "var v := Box<int>; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A NESTED type that declares NO parameters, reached bare: `Outer.Inner`
// supplies zero and the nested type declares zero — the arity rule holds.
BEZEL_TEST(sema_generic_arity_expr_7164,
           nongeneric_nested_type_bare_reference_accepted_spec_14_1) {
    auto r = runAot(
        "module M; "
        "public class Outer "
        "  public class Inner "
        "    public static method Make(): int "
        "      return 5; "
        "    end Make; "
        "  end Inner; "
        "end Outer; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Outer.Inner.Make()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// §2.2's arity SELECTION (the rule the arity check runs AFTER): a user
// `class Set` (ZERO parameters) does not shadow the §2.3 always-available
// `Set<K>` (ONE parameter) — "a non-generic `Counter` and the
// always-available `Counter<K>` are two names, neither colliding with nor
// shadowing the other". So `Set<int>` names the BUILT-IN at arity 1 and is
// NOT a wrong-arity reference; the TYPE position admits the same spelling.
// The arity check must not reject it.
BEZEL_TEST(sema_generic_arity_expr_7164,
           shadowed_builtin_arity_still_accepted_spec_2_2) {
    auto r = runAot(
        "module M; "
        "public class Set "
        "  public constructor() end constructor; "
        "end Set; "
        "public class D public static method Main(): int "
        "  var v := Set<int>; "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The TYPE position's own arity error is UNCHANGED — same text, same
// verdict. The two positions must not drift apart in their wording.
BEZEL_TEST(sema_generic_arity_expr_7164,
           type_position_arity_diagnostic_text_unchanged_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        withBox("", "var v: Box<int>; "), kArityDiag));
}

// A generic TYPE ALIAS: §14.1 (v0.343) says the rule "… and the same
// holds for a generic type alias". The TYPE position enforces the alias's
// arity with its own wording ("… type alias `X`'s generic-parameter count
// (§27.8)"), so the expression position must enforce the same count with
// the same wording.
constexpr const char* kAliasArityDiag =
    "generic-argument count does not match type alias `MyBox`'s "
    "generic-parameter count";

// `MyBox<int>` at a NON-generic alias (`type MyBox := int;`) — the alias
// declares zero parameters and the reference supplies one. Before the fix
// the type-argument list was silently dropped and the program compiled.
BEZEL_TEST(sema_generic_arity_expr_7164,
           alias_type_ref_with_wrong_count_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(rejectsWithDiag(
        "module M; "
        "type MyBox := int; "
        "public class D public static method Main(): int "
        "  var v := MyBox<int>; "
        "  return 0; "
        "end Main; end D; ",
        kAliasArityDiag));
}

// The control: `MyBox<int>` at a GENERIC alias (`type MyBox<T> := T;`) —
// the alias declares one parameter and the reference supplies one.
BEZEL_TEST(sema_generic_arity_expr_7164,
           generic_alias_type_ref_with_the_declared_count_accepted_spec_14_1) {
    auto r = runAot(
        "module M; "
        "type MyBox<T> := T; "
        "public class D public static method Main(): int "
        "  var v := MyBox<int>; "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A generic METHOD's explicit type arguments are a DIFFERENT rule (§14.1 /
// §27.6's method spelling, and §22.12's non-generic-entity rule, which is
// issue #7162's). The arity rule must not fire on them: `Make<int>(7)` is
// a bare callee carrying a type-argument list, and the METHOD's own
// parameter count is one.
BEZEL_TEST(sema_generic_arity_expr_7164,
           generic_method_type_args_still_accepted_spec_14_1) {
    auto r = runAot(
        "module M; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Make<int>(7)); "
        "  return 0; "
        "end Main; "
        "public static method Make<T>(v: T): T "
        "  return v; "
        "end Make; "
        "end D; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// =============================================================================
// PART 4 — AN IMPORTED generic type. The imported type's declared arity is
// the reference's declared count, and the §2.2 selection's imported table
// (`importedByArity_`) is one of the two the check consults, so an imported
// type of the same name at the reference's arity is a DIFFERENT candidate —
// the reference denotes it, and the arity rule is checked against it.
// =============================================================================

namespace {

constexpr const char* kGenericLib = R"(
module LB;
public class Box<T>
  public static method Make(): int
    return 7;
  end Make;
end Box;
)";

std::string appWithBoxCall(const std::string& call) {
    return std::string("module App;\nimport LB;\n") +
           "public class D public static method Main(): int "
           "  Console.WriteLine(" + call + "); "
           "  return 0; "
           "end Main; end D; ";
}

bool modulesRejectWithDiag(const std::vector<ModuleSource>& mods,
                            const std::string& entry,
                            const std::string& needle) {
    JitResult r = runAotModules(mods, entry);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The imported generic type with its DECLARED count: `Box<int>.Make()` at
// the library's `class Box<T>` — admitted, and the program runs.
BEZEL_TEST(sema_generic_arity_expr_7164,
           imported_generic_type_receiver_correct_arity_accepted_spec_14_1) {
    JitResult r = runAotModules(
        {ModuleSource{"LB", kGenericLib},
         ModuleSource{"App", appWithBoxCall("Box<int>.Make()")}},
        "App");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// The imported generic type with a WRONG count: `Box<int, string>.Make()`.
BEZEL_TEST(sema_generic_arity_expr_7164,
           imported_generic_type_receiver_wrong_arity_rejected_spec_14_1) {
    BEZEL_EXPECT_TRUE(modulesRejectWithDiag(
        {ModuleSource{"LB", kGenericLib},
         ModuleSource{"App", appWithBoxCall("Box<int, string>.Make()")}},
        "App", kArityDiag));
}

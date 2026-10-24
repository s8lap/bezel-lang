// fix #7162 — a type-argument list on a NON-GENERIC method was silently
// ignored.
//
// THE DEFECT (measured on the pre-fix tree, at the lane's base SHA 1194f61f).
//
//   §22.12 (v0.343), the generics-vs-less-than resolution, states the rule for
//   every name a syntactically-successful generic reading reaches:
//
//     "The scan is bounded by the enclosing statement and commits no semantic
//      judgment — a name that resolves to a non-generic entity after a
//      syntactically-successful generic reading is an ordinary error at that
//      name."
//
//   §14.1 (v0.343) fixes the arity of such a reference:
//
//     "Every such reference supplies exactly the declared number of type
//      arguments."
//
//   §14.5 (v0.343) states the all-or-nothing rule for a CALL's explicit list:
//
//     "An explicit type-argument list binds the declared type parameters in
//      declaration order and must supply exactly as many arguments as the
//      method declares type parameters; a shorter or longer list is a compile
//      error — there are no partial lists."
//
//   A method that declares NO type parameters of its own is a non-generic
//   entity, and a list of one is not "exactly the declared number" (zero). The
//   compiler nevertheless accepted the list and DROPPED it:
//
//     `o.Plain<int>()`  ran as `o.Plain()`   (rc 0, stdout 3)
//     `Make<int>(7)`    ran as `Make(7)`     (rc 0, stdout 7)
//     `o.Plain<int>()`  over an `extension Box` block — same (rc 0, stdout 3)
//
//   The METHOD-REFERENCE spelling of the same name was already rejected
//   (§27.6: "a reference to a generic method names exactly its declared type
//   parameters, all or none") — so the gap was the CALL path alone.
//
// WHERE IT LIVED. The call site's §14.5 arity check
// (`ExprTypecheck_Calls.cpp`, `ExprTC::typeCall`) compared the list's length
// with the method's DECLARED type-parameter count, but only under the gate
// `declaredArity > 0` — which is exactly the count a non-generic method has.
// The gate therefore exempted precisely the case §22.12 calls "an ordinary
// error at that name".
//
// The fix removes the gate: the check now fires whenever a list is PRESENT
// and its length differs from the declared count (zero included). It never
// fires on a call without a list, and never on a list that matches.
//
// ORACLE: runAot (the real AOT path) for every row.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The §14.5 arity diagnostic, as the call site emits it. The "declares 0 type
// parameter(s)" wording is the shape this fix adds; the count is the list's
// length and the declared count is the method's own.
constexpr const char* kArityDiagHead = "supplies ";
constexpr const char* kArityDiagTail = "explicit type argument(s), but";

// The §27.6 method-reference diagnostic, which the REFERENCE path already
// emitted before this fix and must keep emitting.
constexpr const char* kMethodRefDiag =
    "the method reference `Plain` names 1 type argument(s), but the method "
    "declares 0 type parameter(s) of its own";

// The §22.12 guard's diagnostic for the member kinds that are not methods.
constexpr const char* kNonGenericEntityDiag = "names no type parameters";

// True iff the program is REJECTED and some ERROR diagnostic contains
// `needle`. Both halves matter: `hasErrors()` alone would pass on an
// unrelated failure, and a matching string on a program that still compiled
// would prove nothing.
bool rejects(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff the program COMPILES and RUNS with the given exit code and
// stdout. The rows that must keep compiling are the risk surface of a change
// that ADDS a rejection, so they are pinned on their OUTPUT, not merely on
// "no error".
bool runs(const std::string& prog, int exitCode, const std::string& stdout) {
    auto r = runAot(prog);
    if (!r.ran) return false;
    if (r.exitCode != exitCode) return false;
    return r.stdoutText == stdout;
}

// A class with a non-generic instance method `Plain` and a generic one `Id`.
std::string withBox(const std::string& stmts) {
    return "module M; "
           "public class Box "
           "  public method Plain(): int "
           "    return 3; "
           "  end Plain; "
           "  public method Id<T>(v: T): T "
           "    return v; "
           "  end Id; "
           "end Box; "
           "public class D public static method Main(): int " +
           stmts + " return 0; end Main; end D; ";
}

}  // namespace

// =============================================================================
// PART 1 — THE DEFECT. A type-argument list on a method that declares no type
// parameters of its own is an ordinary error at that name (§22.12), and its
// arity is wrong (§14.1 / §14.5). Pre-fix these programs COMPILED and RAN,
// with the list silently dropped.
// =============================================================================

// The issue's own shape: `o.Plain<int>()` on a method with no type
// parameters. Pre-fix: rc 0, stdout 3 — the call ran as `o.Plain()`.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_22_12_type_arguments_on_a_non_generic_instance_method_are_an_error) {
    const std::string p = withBox(
        "var o := new Box(); "
        "Console.WriteLine(o.Plain<int>()); ");
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagHead));
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagTail));
}

// The same name in the STATIC spelling (`Box.Plain<int>()`): the callee is
// still a method, so the same rule applies.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_5_type_arguments_on_a_non_generic_static_method_are_an_error) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public static method Plain(): int "
        "    return 3; "
        "  end Plain; "
        "end Box; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Box.Plain<int>()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagHead));
}

// The BARE-CALLEE spelling (`Make<int>(7)`): the callee is an identifier, not
// a member access, so it never reaches the member-access path at all — the
// §14.5 arity check is the only site that can reject it, which is why the fix
// belongs there.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_5_type_arguments_on_a_non_generic_bare_callee_are_an_error) {
    const std::string p =
        "module M; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Make<int>(7)); "
        "  return 0; "
        "end Main; "
        "public static method Make(v: int): int "
        "  return v; "
        "end Make; "
        "end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagHead));
}

// The EXTENSION spelling: an extension method has no parent type, so it is
// reached through the extension lookup rather than the ordinary member
// lookup. Pre-fix: rc 0, stdout 3 — the list was dropped.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_22_12_type_arguments_on_a_non_generic_extension_method_are_an_error) {
    const std::string p =
        "module M; "
        "public class Box "
        "end Box; "
        "public extension Box "
        "  public method Plain(): int "
        "    return 3; "
        "  end Plain; "
        "end; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.Plain<int>()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagHead));
}

// A method of a GENERIC class declares no type parameters OF ITS OWN (§14.1:
// the class's parameters are bound by the receiver's instantiation), so a list
// on the member is the same error. The method-reference spelling of the same
// name was already rejected before this fix; the call spelling was not.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_1_a_method_of_a_generic_class_takes_no_type_arguments_of_its_own) {
    const std::string p =
        "module M; "
        "public class Box<T> "
        "  public method Get(): int "
        "    return 5; "
        "  end Get; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box<int>(); "
        "  Console.WriteLine(o.Get<int>()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kArityDiagHead));
}

// =============================================================================
// PART 2 — THE LEGAL SPELLINGS MUST KEEP COMPILING. A change that ADDS a
// rejection is a risk to every ACCEPT row, so each one is pinned on its
// OUTPUT, not merely on the absence of an error.
// =============================================================================

// §14.1 / §27.6: a generic METHOD's explicit type arguments are meaningful
// and stay accepted.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_1_a_generic_method_call_with_explicit_type_arguments_still_works) {
    const std::string p = withBox(
        "var o := new Box(); "
        "Console.WriteLine(o.Id<int>(7)); ");
    BEZEL_EXPECT_TRUE(runs(p, 0, "7\n"));
}

// §14.5's participation rule: "with a list, only candidates declaring that
// many type parameters of their own take part". A name that overloads a
// non-generic and a generic method keeps the generic candidate for the
// list's arity — and the fix must not reject the call.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_5_an_overloaded_name_keeps_the_generic_candidate_for_the_list) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public method F(x: int): int "
        "    return 1; "
        "  end F; "
        "  public method F<T>(x: T): T "
        "    return x; "
        "  end F; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.F<int>(7)); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(runs(p, 0, "7\n"));
}

// §11.6: a generic NESTED TYPE's own type arguments stay accepted.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_11_6_a_generic_nested_type_with_type_arguments_still_works) {
    const std::string p =
        "module M; "
        "public class Tree<T> "
        "  public class Node<U> "
        "    public static method Create(): int "
        "      return 42; "
        "    end Create; "
        "  end Node; "
        "end Tree; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Tree<int>.Node<int>.Create()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(runs(p, 0, "42\n"));
}

// §27.6: a method REFERENCE to a generic method keeps its own spelling and
// its own diagnostic — the reference path was already guarded before this
// fix and must not be disturbed.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_27_6_a_method_reference_to_a_generic_method_still_works) {
    const std::string p =
        "module M; "
        "public class Util "
        "  public static method Id<T>(v: T): T "
        "    return v; "
        "  end Id; "
        "end Util; "
        "public class D public static method Main(): int "
        "  var f: (int) -> int := Util.Id<int>; "
        "  Console.WriteLine(f(9)); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(runs(p, 0, "9\n"));
}

// The method REFERENCE spelling of the defect's own name keeps ITS OWN
// diagnostic — the §27.6 one, not the call site's §14.5 one. The reference
// path and the call path are different code paths (§27.6 names the type
// arguments on the member name; §14.5 names them at a call), and this fix
// changes only the call path.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_27_6_a_method_reference_to_a_non_generic_method_keeps_its_own_diagnostic) {
    const std::string p = withBox(
        "var o := new Box(); "
        "var f: () -> int := o.Plain<int>; "
        "Console.WriteLine(f()); ");
    BEZEL_EXPECT_TRUE(rejects(p, kMethodRefDiag));
}

// =============================================================================
// PART 3 — THE MEMBER KINDS. §22.12's rule covers every name a generic
// reading reaches. A type-argument list is spelled on a generic METHOD
// (§14.1 / §27.6) and on a NESTED TYPE (§11.6); on every other kind of
// member the list spells nothing, and §7135's guard already rejected those.
// These rows pin that the fix did not disturb them.
// =============================================================================

// A FIELD: the §22.12 guard's diagnostic (unchanged by this fix).
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_22_12_type_arguments_on_a_field_stay_the_non_generic_entity_error) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public F: int; "
        "  public constructor() F := 1; end constructor; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.F<int>); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kNonGenericEntityDiag));
}

// A PROPERTY: the §22.12 guard's diagnostic (unchanged by this fix).
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_22_12_type_arguments_on_a_property_stay_the_non_generic_entity_error) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public property P: int; "
        "  public constructor() P := 1; end constructor; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.P<int>); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, kNonGenericEntityDiag));
}

// A NESTED TYPE that declares no type parameters of its own: the list is a
// wrong arity on a type reference (§14.1), which the compiler already
// rejected before this fix.
BEZEL_TEST(nongeneric_method_type_args_7162,
           spec_14_1_type_arguments_on_a_non_generic_nested_type_stay_rejected) {
    const std::string p =
        "module M; "
        "public class Outer "
        "  public class Inner "
        "    public static method Create(): int "
        "      return 42; "
        "    end Create; "
        "  end Inner; "
        "end Outer; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Outer.Inner<int>.Create()); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(
        p, "generic-argument count does not match type's generic-parameter count"));
}

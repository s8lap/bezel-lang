// fix #7012 / §2.7 (the "Enclosing-type members" paragraph and the normative
// table's Bare type name row), §2.7 "Through nesting", §2.2 (arity), §11.6,
// §13.1 / §13.4 (accessibility), §12.2 (hiding), v0.340.
//
// THE DEFECT. §2.7's ranking paragraph, "Enclosing-type members":
//
//   "Inside a type's bodies, the enclosing type's accessible members
//    (INHERITED INCLUDED; instance and static, ...) participate in rule 1's
//    bare-name lookup at a position fixed by the name's syntactic shape"
//
// and the table's Bare type name row — the normative statement of that
// position — ranks those members "between local scope and module declarations,
// NESTED TYPES ONLY" and says that "a nested type of the name, DECLARED OR
// INHERITED, is the innermost answer (§2.2 selecting by arity)".
// §11.6 names the generic case outright: a type nested in a generic type "is
// named only where §2.7's enclosing-type lookup reaches it ... (`Node<int>` in
// `Tree`'s own declaration OR IN THE BODY OF A TYPE DERIVING FROM `Tree`)".
//
// MEASURED on the base (5c3e10f7), for
//
//   class B { public class N { public method Get(): int return 7; end Get;
//                               end N; end B; }
//   class K: B { ... using var n: N := new N(); ... }
//
// the bare `N` in `K`'s body reported "unresolved type name `N`" — TWICE, once
// per occurrence — with a generic base (`B<T>` / `K: B<int>`) and with a generic
// nested type (`B<T>`'s `N<U>` spelled `N<string>`) failing identically.
// Expected: the program prints `7`.
//
// WHY. A body scope registers only the type's GENERIC PARAMETERS and NESTED
// TYPES (`resolveTypeDecl`'s §11.3/#482 loop), and a nested type's body scope
// parents to the OUTER type's body scope — so LEXICAL nesting has always
// resolved through the Scope chain. Inheritance is a separate axis with its own
// accessor (`baseTypes`), and no Scope carries it: `lookupBareTypeName` walked
// the chain, found nothing, and reported the name unresolved.
//
// The fix adds one arm to that walk: at each enclosing-type link, after the
// link's own declared nested types (already in its scope map) and after the §2.7
// stop, walk the link's base chain breadth-first for a nested type of the name
// whose arity the reference fixes and that §13.1 makes accessible there.
//
// SCOPE — what is deliberately NOT claimed, and why:
//   * The §2.7 STOP is not widened to base types. That is the other half of the
//     same axis and a strictly larger change (the in-tree note above
//     `enclosingTypeMemberBlocksBareName` defers it), and the table's row
//     decides it without this fix: a base's same-named member of any OTHER kind
//     "neither binds nor ends the search" — see
//     `base_member_of_other_kind_neither_binds_nor_ends_spec_2_7`.
//   * The QUALIFIED spelling `K.N` is a third question and is left exactly as
//     the base has it: §11.6 gives a nested type its DECLARING path
//     (`Outer.Nested`) and §2.8.3 rule 3 identifies a type by its declaring
//     qualified name, so nothing here decides whether the derived type's own
//     name may also lead to it. Pinned unchanged by
//     `qualified_through_derived_type_unchanged_open_question`, so the deferral
//     is visible rather than silent.
//   * §11.6's EFFECTIVE-VISIBILITY clamp ("the more restrictive of its declared
//     visibility and its enclosing class's visibility") needs no arm: the one
//     shape it could bite, a non-public type enclosing the declaring class,
//     is unreachable from another module before any member is considered —
//     MEASURED, `internal class Outer` wrapping the declaring class, the
//     importing module fails with "module `Lib` has no public type `Outer`
//     (§2.7)". The same-module case is one where every type involved is already
//     visible under §13.1.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The issue's literal repro, and the shape every later row varies.
constexpr const char* kIssueRepro = R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)";

bool hasErrorWith(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ============================================================================
// Part 1 — #7012's literal repro. §2.7's Bare type name row admits an
// INHERITED nested type as "the innermost answer"; the base rejected it as an
// unresolved name. The stdout assertion is the point: it reads the value out
// of the base's nested type, so a row that bound some other `N` fails.
// ============================================================================

BEZEL_TEST(inherited_nested_type_bare_name_7012,
           derived_body_names_inherited_nested_type_spec_2_7) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.7 "Through nesting": the same lookup one link DEEPER — a nested type in
// the derived type's body sees the base's nested type, at the second link of
// the chain ("the first type that has a nested type of the name, declared or
// inherited, ... ends the search there").
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           nested_type_in_derived_body_names_inherited_nested_type_spec_2_7) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public class Inner
    public static method Run(): int
      using var n: N := new N();
      Console.WriteLine(n.Get());
      return 0;
    end Run;
  end Inner;
end K;
class D
  public static method Main(): int return K.Inner.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.7 "Through nesting" again, two links deep in the lexical chain — the
// chain is "the nested type and then each type enclosing it, innermost first",
// so `K.Inner.Deep`'s body walks Inner, K, then the module, and the base's
// nested type is reached at K's link.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           two_link_nested_body_names_inherited_nested_type_spec_2_7) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public class Inner
    public class Deep
      public static method Run(): int
        using var n: N := new N();
        Console.WriteLine(n.Get());
        return 0;
      end Run;
    end Deep;
  end Inner;
end K;
class D
  public static method Main(): int return K.Inner.Deep.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// The BASE chain is walked, not just its first link: the nested type is
// declared on the grandparent. §2.7 says "declared or inherited" of the
// enclosing type, and inheritance is transitive (§12.1).
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           grandbase_nested_type_reached_through_two_base_links_spec_2_7) {
    auto r = runAot(R"(
module M;
class A
  public class N
    public method Get(): int return 7; end Get;
  end N;
end A;
class B: A
end B;
class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ============================================================================
// Part 2 — §11.6's generic-base case, named in the issue and in the spec.
// §11.6: a type nested in a generic type "is named only where §2.7's
// enclosing-type lookup reaches it ... (`Node<int>` in `Tree`'s own declaration
// or in the body of a type deriving from `Tree`)". The base rejected BOTH of
// the rows below.
// ============================================================================

// A generic BASE with a non-generic nested type. The bare name reaches it: a
// bare reference supplies arity 0, which fixes the nested type's arity 0.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           derived_body_names_inherited_nested_type_under_generic_base_spec_11_6) {
    auto r = runAot(R"(
module M;
class B<T>
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B<int>
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// A generic base whose nested type is itself generic, reached WITH type
// arguments. §2.7 "Through nesting" + §2.2: "a reference resolves to the
// first, in that rule's order, whose arity its type-argument list fixes" — one
// supplied argument fixes `N<U>`'s arity 1. `n.V` is a `U`, so this row also
// reads back the SUBSTITUTION: the bare `string` prints, not a `0` from an
// unsubstituted cell. (`N<U>` cannot use `B`'s `T` — §11.6 — so the payload
// rides a property of the nested type's own parameter.)
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           derived_body_names_inherited_generic_nested_type_spec_2_2) {
    auto r = runAot(R"(
module M;
class B<T>
  public class N<U>
    public property V: U;
    public constructor(v: U) V := v; end constructor;
  end N;
end B;
class K: B<int>
  public static method Run(): int
    using var n: N<string> := new N<string>("ok");
    Console.WriteLine(n.V);
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// ============================================================================
// Part 3 — §13.1 / §13.4 / §11.6 accessibility. The arm admits a NESTED TYPE
// only when the level makes it reachable at the site; the levels are decided
// per §13.1, not by name.
// ============================================================================

// §13.1: "`protected` — The declaring class and all classes derived from it".
// `K` is derived from `B`, so `B`'s protected nested type is reachable in `K`'s
// body. Same shape rejected before the fix.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           protected_nested_type_of_base_reachable_spec_13_1) {
    auto r = runAot(R"(
module M;
class B
  protected class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §13.1 + §11.6: "`private` is lexical as well as nominal: §11.6 extends it
// between a class and the types nested in it, at any depth and in both
// directions", and §12.2's carve-out — "a derived class nested inside the base
// class (§11.6) sees the base's `private` members". `B.K` is nested inside `B`,
// so `B`'s private nested type is reachable from it. The same `class K: B` at
// TOP LEVEL is the next row.
//
// NOT a fail-without row, and the reason is worth recording: this shape
// already worked on the base, because `resolveMember` gives a nested type's
// body scope the OUTER type's body scope as its parent — so `B.K`'s body walks
// the Scope chain into `B`'s own body scope, which carries `N` as a DECLARED
// nested type. §11.6's lexical grant needs no inheritance arm, and this row
// pins that it still holds with one in place.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           private_nested_type_reachable_from_nested_derived_spec_11_6) {
    auto r = runAot(R"(
module M;
class B
  private class N
    public method Get(): int return 7; end Get;
  end N;
  public class K: B
    public static method Run(): int
      using var n: N := new N();
      Console.WriteLine(n.Get());
      return 0;
    end Run;
  end K;
end B;
class D
  public static method Main(): int return B.K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §13.1: "`private` — The declaring class only". A TOP-LEVEL `K: B` is not
// nested inside `B` and does not derive-nest, so `B`'s private nested type is
// NOT reachable — the name stays unresolved. The arm filters this candidate and
// keeps looking; it does not admit and then complain.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           private_nested_type_of_base_not_reachable_spec_13_1) {
    auto r = runAot(R"(
module M;
class B
  private class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §13.1 / §13.4 across a module boundary: `internal` is "code in the same
// module", so an imported base's `internal` nested type is out of reach, while
// its `public` one is in. Two modules, one entry.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           imported_base_internal_nested_type_not_reachable_spec_13_4) {
    const std::string lib = R"(
module Lib3;
public class Root
  internal class N
    public method Get(): int return 11; end Get;
  end N;
end Root;
public class Mid: Root
end Mid;
)";
    const std::string app = R"(
module App3;
import Lib3;
public class K: Lib3.Mid
  public static method Main(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end K;
)";
    JitResult r = runAotModules({ModuleSource{"Lib3", lib},
                                 ModuleSource{"App3", app}},
                                "App3");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The other half of that pair, so the `internal` row above is a LEVEL test and
// not "the imported base was not searched at all": the same imported base's
// `public` nested type resolves and RUNS. Also covers the base chain across the
// artifact boundary (`Root`'s nested type reached from `Mid`'s subclass).
//
// NEITHER cross-module row is a fail-without row — MEASURED, both verdicts are
// the BASE's, because ModuleLoader already exports only the members the
// artifact's visibility admits and re-registers the nested type for the
// importing module. They are the control that keeps this fix from changing the
// imported path, and the `internal` row is what proves the LEVEL is still what
// decides it.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           imported_base_public_nested_type_reachable_spec_2_7) {
    const std::string lib = R"(
module Lib2;
public class Root
  public class N
    public method Get(): int return 11; end Get;
  end N;
end Root;
public class Mid: Root
end Mid;
)";
    const std::string app = R"(
module App2;
import Lib2;
public class K: Lib2.Mid
  public static method Main(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end K;
)";
    JitResult r = runAotModules({ModuleSource{"Lib2", lib},
                                 ModuleSource{"App2", app}},
                                "App2");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// ============================================================================
// Part 4 — the RANKING, and what the arm must NOT do. §2.7's Bare type name
// row ranks the enclosing type's members "between local scope and module
// declarations" — so an inherited nested type OUTRANKS a same-named module
// declaration, and the row below reads back which of the two it bound.
// ============================================================================

// Two same-named types, one nested in the base and one at module scope. The
// enclosing type's members rank ABOVE module declarations, so the base's `N`
// (returning 7) is the answer and the module's `N` (a record whose `V` is 9)
// is not. The stdout assertion is what distinguishes them.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           inherited_nested_type_outranks_module_declaration_spec_2_7) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
record N(V: int);
class K: B
  public class Inner
    public static method Run(): int
      using var n: N := new N();
      Console.WriteLine(n.Get());
      return 0;
    end Run;
  end Inner;
end K;
class D
  public static method Main(): int return K.Inner.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.7's Bare type name row's last clause: "a same-named member of any other
// kind — on the type's own body or on a type enclosing it, DECLARED OR
// INHERITED — neither binds nor ends the search". A base's `public method N`
// therefore neither satisfies the bare type name nor stops the search: the
// module's `record N` is what binds. Unchanged by the fix, and pinned because
// the arm runs at the same link as the §2.7 stop.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           base_member_of_other_kind_neither_binds_nor_ends_spec_2_7) {
    auto r = runAot(R"(
module M;
record N(V: int);
class B
  public method N(): int return 7; end N;
end B;
class K: B
  public static method Run(): int
    var n := new N(9);
    Console.WriteLine(n.V);
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// §12.2: "Declaring a member in a derived class with the same name as ANY
// inherited member visible at the derived class's declaration site is a compile
// error". The derived type redeclaring the base's nested type is still
// rejected, with the §12.2 diagnostic and not the unresolved-name one — the
// fix must not turn a hiding error into a silent second `N`.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           hiding_of_inherited_nested_type_still_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public class N
    public method Get(): int return 9; end Get;
  end N;
end K;
class D
  public static method Main(): int return 0; end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "hides an inherited member of the same name"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "\xC2\xA7""12.2"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.7 "Through nesting" + §2.2: the candidate is "the first type that has a
// nested type of the name, declared or inherited, WHOSE ARITY THE REFERENCE
// FIXES". A bare `N` supplies no type argument, so it does not fix `N<U>`'s
// arity 1 — the candidate is skipped rather than bound, and the search ends
// with the name unresolved. This pins that the arm filters on arity instead of
// handing a mismatched nested type to a later arity check.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           arity_mismatched_inherited_nested_type_is_not_a_candidate_spec_2_2) {
    auto r = runAot(R"(
module M;
class B
  public class N<T>
    public method Get(): T return default(T); end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine("reached");
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.7's table, Bare type name row: the enclosing type's members rank
// "between local scope and module declarations" — ABOVE local scope would be
// wrong the other way, and this row pins the direction from the other side: a
// method's own type parameter outranks the base's nested type of the name
// (§2.7 rule 1: "An inner declaration's type parameter therefore outranks an
// enclosing type's nested type of the name").
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           local_type_parameter_outranks_inherited_nested_type_spec_2_7) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run<T>(v: T): string
    using var n: N := new N();
    return v.ToString();
  end Run;
end K;
class D
  public static method Main(): int
    Console.WriteLine(K.Run(3));
    return 0;
  end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ============================================================================
// Part 5 — the HELD half, pinned so the deferral is visible.
// ============================================================================

// The QUALIFIED spelling `K.N` — through the DERIVED type — is unchanged from
// the base. §11.6 gives a nested type its DECLARING path (`Outer.Nested`) and
// §2.8.3 rule 3 identifies a type by its declaring module name, qualified type
// name and generic arity, so whether the derived type's own name may also lead
// to it is NOT decided by the text this fix cites. MEASURED alongside: the
// base ALREADY admits an inherited `static` member through the derived type
// (`K.S()` compiles), which is the tension a ruling has to settle.
//
// THIS ROW PINS THE BASE'S VERDICT SO THE DEFERral IS VISIBLE, not because
// §11.6 decides it. A ruling that admits `K.N` flips this row to an accept and
// updates `imported_base_public_nested_type_reachable_spec_2_7`'s sibling
// shapes; nothing else in this suite moves.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           qualified_through_derived_type_unchanged_open_question) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: K.N := new K.N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "type `K` has no nested type named `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}
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
// fix #7084 (§2.2, Appendix C.1, v0.343) adds §2.2's arity FALLBACK to that
// arm: a candidate whose arity the reference does not fix is not the one the
// arity rule selects, but it IS one of the candidates, and "where none has
// that arity, it resolves to the first of them and is Appendix C.1's arity
// error" — so the reference resolves to it and the diagnostic is the arity
// error, the same one the LEXICAL twin of that shape gives. The rows
// `arity_mismatched_inherited_nested_type_is_the_arity_error_spec_2_2` and
// `arity_mismatched_inherited_nested_type_with_arguments_is_the_arity_error_spec_2_2`
// pin it; the row
// `arity_mismatched_inherited_nested_type_is_not_a_candidate_spec_2_2` pinned
// the divergent "unresolved type name `N`" and is FLIPPED by that fix.
//
// fix-forward of #7084 (§2.2 / §2.3, v0.343) corrects the fallback's
// PRECONDITION. §2.2's fallback applies "where none has that arity", and two
// of the rule-1 candidate sources are not visible to the enclosing-type walk
// that finds the inherited nested type: the §2.3 always-available names and
// the §2.2 ambiguous-import set. Binding the inherited candidate before they
// are consulted made an inherited `Task<T>` steal the always-available
// `Task` (a §2.3 guarantee lost) and swallowed §2.2's collision diagnostic.
// Part 6 pins the three shapes that decide it.
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

// §2.7 "Through nesting" + §2.2 (the arity rule). A bare `N` supplies no type
// argument, so it does not fix `N<U>`'s arity 1 — `N<U>` is not the candidate
// the arity rule SELECTS. But it IS one of the candidates, and §2.2's
// fallback is explicit about what happens when none of them has the
// reference's arity:
//
//   "where none has that arity, it resolves to the first of them and is
//    Appendix C.1's arity error, never an alias, a member or a module prefix"
//
// So the reference resolves to `N<U>` and is Appendix C.1's arity error — the
// SAME diagnostic the LEXICAL twin of this shape gives (a generic nested type
// in a type's OWN body, referenced without type arguments). Before fix #7084
// the inherited shape reported "unresolved type name `N`" — LESS specific
// than its lexical twin, and not the diagnostic §2.2 names for it.
//
// This row is the FLIP of the #7012 control row
// `arity_mismatched_inherited_nested_type_is_not_a_candidate_spec_2_2`, which
// pinned the divergent "unresolved type name `N`".
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           arity_mismatched_inherited_nested_type_is_the_arity_error_spec_2_2) {
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
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    // The name is RESOLVED (to the inherited `N<U>`), so it is not reported
    // as an unresolved name any more.
    BEZEL_EXPECT_FALSE(hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 + Appendix C.1 (v0.343): the OTHER direction of the same arity
// mismatch — an inherited NON-generic nested type referenced WITH type
// arguments. The reference fixes arity 1, the candidate declares arity 0, so
// the candidate is not the one §2.2 selects; nothing else has the name, so
// the reference resolves to it and is Appendix C.1's arity error. The LEXICAL
// twin (a non-generic nested type in a type's OWN body, referenced as
// `N<string>`) gives the same diagnostic.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           arity_mismatched_inherited_nested_type_with_arguments_is_the_arity_error_spec_2_2) {
    auto r = runAot(R"(
module M;
class B
  public class N
    public method Get(): int return 7; end Get;
  end N;
end B;
class K: B
  public static method Run(): int
    using var n: N<string> := new N<string>();
    Console.WriteLine("reached");
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    BEZEL_EXPECT_FALSE(hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.7 "Through nesting" + §2.2 (v0.343): a base that has a member of the
// name which is NOT a nested type is not a candidate at all — §2.7's rule is
// about "a NESTED TYPE of the name". So there is no candidate of any arity,
// §2.2's fallback has nothing to resolve to, and the name is unresolved — the
// diagnostic is unchanged by fix #7084.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           base_member_of_another_kind_is_not_an_arity_candidate_spec_2_7) {
    auto r = runAot(R"(
module M;
class B
  public method N(): int return 2; end N;
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
    BEZEL_EXPECT_FALSE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
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
// ============================================================================
// Part 6 — the arity FALLBACK's two other rule-1 candidate sources
// (fix-forward of #7084, §2.2 / §2.3, v0.343).
//
// §2.2's fallback — "where none has that arity, it resolves to the first of
// them and is Appendix C.1's arity error" — has a PRECONDITION: that no
// candidate has the reference's arity. Two of the candidate sources are not
// visible to the enclosing-type walk that finds the inherited nested type:
//
//   * the §2.3 ALWAYS-AVAILABLE names, which the name resolver resolves after
//     the walk finds nothing; and
//   * the §2.2 AMBIGUOUS-IMPORT set, whose collision diagnostic the name
//     resolver reports after the walk finds nothing.
//
// So the fallback may bind the inherited candidate only after both have been
// consulted. The rows below pin the three shapes that decide it.
// ============================================================================

// §2.2 (the arity rule) + §2.3 (the always-available names), v0.343: the
// `Task` family is ALWAYS AVAILABLE — "Importing `System.Threading` is not
// required to use the `Task` family" (§2.3) — and `Task` is available at
// arity 0 (`Task`) AND at arity 1 (`Task<T>`) (§2.3 Row 5), which the
// compiler records as the `kBuiltinTaskLike` sentinel.
//
// A base that declares a nested `Task<T>` must NOT take the name away. §2.2
// selects "the first, in that rule's order, whose arity its type-argument list
// fixes", and the always-available `Task` HAS the reference's arity (0), so
// the fallback's precondition ("where none has that arity") is FALSE and the
// fallback must not fire. §2.7 rule 1's own worked example is the same shape:
// "`Array<int>` passes over it to the always-available `Array<T>`" — an
// arity-mismatched candidate is PASSED OVER, not fatal.
//
// The row pins the IDENTITY, not merely the absence of a diagnostic: a bare
// `Task` reference can only type-check if it denotes a type of arity 0, and
// the inherited `Task<T>` is arity 1 — so a program that names the type bare
// and compiles at all has denoted the ALWAYS-AVAILABLE `Task`.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           always_available_task_wins_over_an_inherited_nested_type_spec_2_3) {
    auto r = runAot(R"(
module M;
class B
  public class Task<T>
    public method Get(): int return 1; end Get;
  end Task;
end B;
class K: B
  public method Take(t: Task): int return 0; end Take;
  public static method Run(): int
    Console.WriteLine("reached");
    return 0;
  end Run;
end K;
class D
  public static method Main(): int return K.Run(); end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    BEZEL_EXPECT_FALSE(hasErrorWith(r, "unresolved type name `Task`"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "reached\n");
}

// §2.3 Row 5 + §2.6: `TaskCompletionSource` is the SECOND always-available
// name whose arity is the sentinel (available at arity 0 and at arity 1), so
// it is the second half of the pair the row above pins. Without the sentinel
// being read as "arity 0 AND 1", the always-available name is not a candidate
// of the reference's arity and the fallback steals the name.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           always_available_task_completion_source_wins_over_an_inherited_nested_type_spec_2_3) {
    auto r = runAot(R"(
module M;
class B
  public class TaskCompletionSource<T>
    public method Get(): int return 1; end Get;
  end TaskCompletionSource;
end B;
class K: B
  public method Take(t: TaskCompletionSource): int return 0; end Take;
end K;
class D
  public static method Main(): int return 0; end Main;
end D;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// §2.2 (the collision rule), v0.343: "When two imported modules export the
// same public name, the bare name is ambiguous: using it unqualified is a
// compile error citing both origins." The two imported `N`s are arity 0 and the
// reference fixes arity 0, so §2.2's arity rule selects them — and they are
// ambiguous. The inherited `N<U>` (arity 1) is not selected, so the fallback
// must not fire and swallow the collision diagnostic.
//
// The collision's TEXT is the pre-existing sibling site's ("multiple imported
// modules export a type with this name and arity") — the site that omits the
// origins is a separate, already-filed gap, so this row pins the parts of the
// message that are stable across that fix: the "bare name `N` is ambiguous"
// prefix and the remedy.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           ambiguous_import_collision_is_not_swallowed_by_the_arity_fallback_spec_2_2) {
    const std::string la = R"(
module LA;
public class N
  public method Get(): int return 77; end Get;
end N;
)";
    const std::string lb = R"(
module LB;
public class N
  public method Get(): int return 88; end Get;
end N;
)";
    const std::string app = R"(
module App;
import LA;
import LB;
class B
  public class N<U>
    public method Get(): int return 8; end Get;
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
    JitResult r = runAotModules({ModuleSource{"LA", la},
                                 ModuleSource{"LB", lb},
                                 ModuleSource{"App", app}},
                                "App");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "bare name `N` is ambiguous"));
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "qualify it via a module-qualified name or an "
                        "import alias"));
    BEZEL_EXPECT_FALSE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    BEZEL_EXPECT_FALSE(hasErrorWith(r, "unresolved type name `N`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 / §2.3, v0.343: the always-available names are BROADER than the
// compiler's built-in table — `Console`, `Math`, `Convert`, `Environment` and
// `BitReader` are always-available names of the auto-imported `System` (§2.3
// Row 10 / stdlib §4) that the built-in table does not carry, so the name
// resolver does not resolve them at all (a PRE-EXISTING gap, not this fix's).
//
// The consequence is NAMED here, because it is a diagnostic this fix moves:
// for such a name the fallback has nothing to defer to, so the reference
// resolves to the inherited nested type and the diagnostic is Appendix C.1's
// arity error — where the base reported "unresolved type name `Console`".
// The verdict is unchanged (a rejection either way); the MESSAGE moves, and
// this row pins it so the move is not silent.
BEZEL_TEST(inherited_nested_type_bare_name_7012,
           always_available_module_member_name_keeps_the_arity_fallback_message_spec_2_2) {
    auto r = runAot(R"(
module M;
class B
  public class Console<T>
    public method Get(): int return 1; end Get;
  end Console;
end B;
class K: B
  public method Take(c: Console): int return 0; end Take;
end K;
class D
  public static method Main(): int return 0; end Main;
end D;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    BEZEL_EXPECT_FALSE(hasErrorWith(r, "unresolved type name `Console`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

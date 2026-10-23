// fix #7013 / §2.2 (the "Collisions" paragraph — arity selection), §11.6
// ("A bare enclosing segment fixes arity 0"), §2.7 (rule 1), v0.340.
//
// THE DEFECT. §2.2's arity rule:
//
//   "names are compared together with their generic arity ... Arity selects
//    among the types rule 1 of §2.7 finds for an identifier: a reference
//    resolves to the first, in that rule's order, whose arity its
//    type-argument list fixes"
//
// and §2.2's own example names the shape this file pins:
//
//   "a non-generic `Counter` and the always-available `Counter<K>` are two
//    names, neither colliding with nor shadowing the other (so a file
//    importing a library's `Counter` uses both `Counter.Bump()` and
//    `Counter<string>`)"
//
// — `Counter.Bump()` is a CHAIN HEAD. §11.6 says the same about the head
// specifically: "A bare enclosing segment fixes arity 0", and A.5
// (`TypeNoNullable ::= QualifiedName GenericArgs?`) admits a type-argument
// list after the LAST segment only, so the head of a dotted type name is
// ALWAYS arity 0.
//
// So with a library exporting a non-generic `C` and the importing module
// declaring a generic `C<T>`, the two are "neither colliding with nor
// shadowing the other" — a name of DIFFERENT arity does not shadow — and the
// chain head `C.N` (arity 0) selects the library's non-generic `C`, whose
// nested `N` the program then uses.
//
// MEASURED on the base (a7b8d33d), the issue's literal repro reported
//
//   "generic-argument count does not match type's generic-parameter count:
//    `C` declares 1 type parameter, and the bare enclosing segment in `C.N`
//    supplies none, so no type name passes through it ... (§11.6,
//    Appendix C.1, v0.340)"
//
// — the head resolved to the local `C<T>`, and §11.6's "In a type position,
// therefore, no name passes through a generic enclosing type" made that an
// error. The program should RUN and print 7.
//
// WHY THE LOCAL DECLARATION DID NOT SHADOW THE IMPORT. §2.2 says so
// expressly: "For this collision rule and the shadowing rule below, names
// are compared together with their generic arity" — the shadowing rule
// ("A declaration in the current module shadows any same-named import for
// bare-name purposes") is arity-aware too, so `C<T>` (arity 1) does not
// shadow `C` (arity 0). The import loop in TypeResolution.cpp keys its
// shadowing test on the BARE name, so the imported arity-0 `C` never reached
// the scope chain; the fix reaches it directly.
//
// SCOPE — what is deliberately NOT claimed, and why:
//   * A FULLY BARE reference to the same pair (`var c: C := ...` at a local
//     `C<T>`) is the same §2.2 rule at a different site — the bare-name
//     lookup, where the enclosing-type-member axis also participates — and is
//     NOT changed here. It is pinned unchanged by
//     `bare_reference_to_a_generic_local_is_still_an_arity_error_spec_2_2`.
//   * The VALUE position's chain head (`C.S()`, a static member of the
//     imported non-generic `C`) is the same §2.2 rule, but it is diagnosed in
//     `src/sema/ExprTypecheck_Calls.cpp` — a file this lane does not own.
//     Pinned unchanged by `static_member_of_the_imported_head_unchanged_spec_22_12`.
//   * The always-available names (§2.3 Row 10) are left out of the arity
//     selection: the (identifier, arity) keying for that set is an open
//     question (#5140's HOLD), and this fix does not decide it.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The library the issue's repro imports: a public, NON-GENERIC `C` with a
// public nested `N`.
constexpr const char* kLib = R"(
module LC;
public class C
  public class N
    public method Get(): int return 7; end Get;
  end N;
end C;
)";

// The app's own generic `C<T>`, which §2.2 says does not shadow the
// library's non-generic `C`.
constexpr const char* kAppBody = R"(
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    using var n: C.N := new C.N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";

bool hasErrorWith(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff a §11.6 / Appendix C.1 arity error about a generic enclosing
// segment is present — the base's verdict for this shape.
bool hasEnclosingSegmentArityError(const JitResult& r) {
    return hasErrorWith(r, "the bare enclosing segment in") &&
           hasErrorWith(r, "\xC2\xA7""11.6");
}

// ============================================================================
// Part 1 — the defect. A chain head fixes arity 0, so it selects the
// imported non-generic `C` and the program RUNS.
// ============================================================================

// §2.2 / §11.6 (v0.340): the issue's literal repro. On the base this
// reported the §11.6 arity error; the program must compile and print 7.
BEZEL_TEST(chain_head_arity_selection_7013,
           imported_nongeneric_head_selected_spec_2_2) {
    const std::string app = std::string("module App;\nimport LC;\n") +
                            kAppBody;
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App", app}},
                                "App");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §11.6 (v0.340): the head reaches a nested type TWO levels down
// (`C.N.M`) — the arity-0 selection is about the head, and the chain below
// it is §11.3's ordinary nested-type walk.
BEZEL_TEST(chain_head_arity_selection_7013,
           imported_nongeneric_head_reaches_deep_nested_type_spec_11_3) {
    const std::string lib = R"(
module LC2;
public class C
  public class N
    public class M
      public method Get(): int return 13; end Get;
    end M;
  end N;
end C;
)";
    const std::string app = R"(
module App2;
import LC2;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    using var m: C.N.M := new C.N.M();
    Console.WriteLine(m.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC2", lib},
                                 ModuleSource{"App2", app}},
                                "App2");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "13\n");
}

// §2.2 (v0.340): the same selection in a FIELD's declared type — the head
// is spelled the same way, and §2.2's rule is about the reference, not the
// position it appears in.
BEZEL_TEST(chain_head_arity_selection_7013,
           imported_nongeneric_head_in_a_field_type_spec_2_2) {
    const std::string app = R"(
module App3;
import LC;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public property P: C.N;
  public constructor(p: C.N) P := p; end constructor;
  public static method Main(): int
    var d := new D(new C.N());
    Console.WriteLine(d.P.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App3", app}},
                                "App3");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.2 (v0.340): the head's selection also holds for a TYPE ALIAS's
// underlying type — the alias body is resolved in the same scope, and the
// head is spelled the same way.
BEZEL_TEST(chain_head_arity_selection_7013,
           imported_nongeneric_head_in_an_alias_body_spec_2_2) {
    const std::string app = R"(
module App4;
import LC;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
type Holder := C.N;
class D
  public static method Main(): int
    using var n: Holder := new Holder();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App4", app}},
                                "App4");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.2 (v0.340): TWO imports exporting the same non-generic `C` are the
// §2.2 collision at that arity — the arity-0 selection has two candidates
// and neither wins, so the reference is ambiguous. (On the base this
// reported the §11.6 arity error instead: the head never reached the
// imports, so the collision was invisible.)
BEZEL_TEST(chain_head_arity_selection_7013,
           ambiguous_imported_heads_rejected_spec_2_2) {
    const std::string libA = R"(
module LA;
public class C
  public class N
    public method Get(): int return 21; end Get;
  end N;
end C;
)";
    const std::string libB = R"(
module LB;
public class C
  public class N
    public method Get(): int return 22; end Get;
  end N;
end C;
)";
    const std::string app = std::string("module App5;\nimport LA;\nimport LB;\n") +
                            kAppBody;
    JitResult r = runAotModules({ModuleSource{"LA", libA},
                                 ModuleSource{"LB", libB},
                                 ModuleSource{"App5", app}},
                                "App5");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "is ambiguous: multiple imported modules "
                                       "export a type with this name and arity"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "\xC2\xA7""2.2"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// Part 2 — CONTROLS. The arity-0 selection must not change any of these.
// ============================================================================

// §2.2 (v0.340): a local NON-GENERIC `C` and an imported non-generic `C`
// are the SAME (name, arity) — §2.2's shadowing rule applies, and the local
// declaration wins. The program prints the LOCAL nested type's value.
BEZEL_TEST(chain_head_arity_selection_7013,
           local_nongeneric_head_still_wins_spec_2_2) {
    const std::string app = R"(
module App6;
import LC;
class C
  public class N
    public method Get(): int return 4; end Get;
  end N;
end C;
class D
  public static method Main(): int
    using var n: C.N := new C.N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App6", app}},
                                "App6");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// §11.6 (v0.340): with NO arity-0 candidate, a generic head is still
// §11.6's arity error — "In a type position, therefore, no name passes
// through a generic enclosing type". Unchanged by this fix.
BEZEL_TEST(chain_head_arity_selection_7013,
           generic_head_without_an_arity0_candidate_is_an_arity_error_spec_11_6) {
    const std::string app = R"(
module App7;
class C<T>
  public class N
    public method Get(): int return 3; end Get;
  end N;
end C;
class D
  public static method Main(): int
    using var n: C.N := new C.N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"App7", app}}, "App7");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnclosingSegmentArityError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 / §11.6 (v0.340): an imported GENERIC `C<T>` is not an arity-0
// candidate — a name still cannot pass through a generic enclosing type.
// The local `C<T>` and the imported `C<T>` are the same (name, arity), so
// the local shadows the import and the head stays generic: the §11.6 arity
// error stands.
BEZEL_TEST(chain_head_arity_selection_7013,
           imported_generic_head_is_not_an_arity0_candidate_spec_2_2) {
    const std::string lib = R"(
module LC3;
public class C<T>
  public class N
    public method Get(): int return 9; end Get;
  end N;
end C;
)";
    const std::string app = std::string("module App8;\nimport LC3;\n") +
                            kAppBody;
    JitResult r = runAotModules({ModuleSource{"LC3", lib},
                                 ModuleSource{"App8", app}},
                                "App8");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasEnclosingSegmentArityError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.7 (v0.340): an ENCLOSING-TYPE MEMBER of the head's name is rule 1's
// earlier position than the current module's declarations, so it wins over
// both the local generic `C<T>` and the imported non-generic `C`. This is
// the control that the arity-0 selection does not overtake an enclosing
// member that already answers the name.
BEZEL_TEST(chain_head_arity_selection_7013,
           enclosing_member_head_outranks_the_import_spec_2_7) {
    const std::string app = R"(
module App9;
import LC;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class Outer
  public class C
    public class N
      public method Get(): int return 6; end Get;
    end N;
  end C;
  public class Inner
    public static method Main(): int
      using var n: C.N := new C.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Outer;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App9", app}},
                                "App9");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// §2.2 (v0.340): the import stays reachable by QUALIFIER — "A declaration
// in the current module shadows any same-named import for bare-name
// purposes (the import stays reachable by qualifier)". `LC.C.N` is that
// qualified spelling and is unchanged by this fix.
BEZEL_TEST(chain_head_arity_selection_7013,
           module_qualified_spelling_unchanged_spec_2_2) {
    const std::string app = R"(
module App10;
import LC;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    using var n: LC.C.N := new LC.C.N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App10", app}},
                                "App10");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ============================================================================
// Part 3 — the DEFERRED shapes, pinned unchanged so the deferral is visible
// rather than silent.
// ============================================================================

// §2.2 (v0.340): a FULLY BARE reference at the same pair is the same
// arity rule at a different site (the bare-name lookup, where the
// enclosing-type-member axis also participates). Not changed here; the
// reference still resolves to the local `C<T>` and is the arity error.
// Follow-up filed as #7079.
BEZEL_TEST(chain_head_arity_selection_7013,
           bare_reference_to_a_generic_local_is_still_an_arity_error_spec_2_2) {
    const std::string app = R"(
module App11;
import LC;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine(c.Id(3));
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App11", app}},
                                "App11");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "generic-argument count does not match type's "
                        "generic-parameter count"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §22.12 (v0.340): the VALUE position's chain head — a static member of
// the imported non-generic `C` — is the same §2.2 rule, but it is
// diagnosed in `src/sema/ExprTypecheck_Calls.cpp`, which this lane does not
// own. Pinned unchanged. Follow-up filed as #7079.
BEZEL_TEST(chain_head_arity_selection_7013,
           static_member_of_the_imported_head_unchanged_spec_22_12) {
    const std::string lib = R"(
module LC4;
public class C
  public static method S(): int return 5; end S;
end C;
)";
    const std::string app = R"(
module App12;
import LC4;
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    Console.WriteLine(C.S());
    return 0;
  end Main;
end D;
)";
    JitResult r = runAotModules({ModuleSource{"LC4", lib},
                                 ModuleSource{"App12", app}},
                                "App12");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorWith(r, "requires type arguments when accessing its static "
                        "members"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "\xC2\xA7""22.12"));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

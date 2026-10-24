// fix #7085 / §2.7 (the normative table's CHAIN HEAD row — "Enclosing-type
// members rank: last, after aliases"), §2.7's warning row, v0.343.
//
// THE DEFECT. §2.7's table is "the normative statement", and its chain-head
// row reads:
//
//   | Chain head — name followed by `.` | last, after aliases | a reachable
//   type outranks a same-named member |
//
// and the prose below the table says:
//
//   "Because the chain-head member fallback ranks below imports, it is the
//    one rule-1 source a later-added import or type declaration can preempt
//    — loudly in practice, since the preempting type must itself host the
//    accessed member — which is what the table's warning row polices."
//
// So at a chain head the ENCLOSING type's accessible member ranks BELOW the
// imported position: a reachable type (here, the library's `C`) outranks the
// enclosing type's same-named member (here, the nested `C` of `Outer`).
//
// MEASURED on the base, the enclosing member WON: the program printed 6 (the
// enclosing `Outer.C`'s nested `N`) where the imported `C`'s nested `N`
// prints 7. Identical on the base and on the tip — pre-existing, not a
// regression.
//
// THE SHAPE. `C.N` is a CHAIN HEAD: the head `C` is followed by `.`, so the
// table's chain-head row governs the head's ranking — NOT the table's
// "Bare type name" row (which is about a name the grammar reads only as a
// `Type`, and whose candidates are nested types only). The head of a dotted
// type name is what §2.7 rule 1 resolves as a bare name, and the row that
// says where the enclosing type's members sit in that resolution is the
// chain-head row.
//
// The chain head here is in a TYPE position (`using var n: C.N := new
// C.N();`), and §2.7's table has no row for "a type position" as such: the
// rows are about the HEAD's syntactic shape, and this head is followed by
// `.`.
//
// THE WARNING ROW is the same rule's other half, and this fix brings it into
// reach in the type path: after the fix the chain head resolves to the
// IMPORTED type while the enclosing type's same-named member is still
// reachable — so §2.7's warning row polices it ("warn, unless the member's
// declared type is the resolved type (or an instantiation of the resolved
// generic)"). The nested type `Outer.C` is not the resolved type, so the
// warning fires.
//
// ORACLES. The BEHAVIOUR rows (what the program prints) use the strict AOT
// oracle (`runAotModules`), the only oracle that runs the real driver. The
// WARNING rows use the sema-only oracle (`parseAndResolveGated`) because the
// AOT oracle CANNOT see a warning: `runAot` fills `r.diagnostics` only on a
// compile REJECT, so a compiler warning on a program that compiles is
// invisible to it (the warning reaches the driver's stderr, which the AOT
// harness captures but does not surface). The sema-only rows use the
// IMPORT-free shape of the same rule (a module-level type beside the
// enclosing type's same-named nested type), because the sema-only oracle
// compiles ONE compilation unit and cannot load an imported module.
//
// The CONTROLS pin what the fix must NOT change: the enclosing member still
// wins when nothing else answers the name (no import), the module-qualified
// spelling still reaches the import, and the current module's own
// declaration still outranks the import (§2.7 rule 1's order — the module's
// declarations come before the imported bare names).

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The library the repro imports: a public, NON-GENERIC `C` with a public
// nested `N` whose `Get()` returns 7.
constexpr const char* kLib = R"(
module LC;
public class C
  public class N
    public method Get(): int return 7; end Get;
  end N;
end C;
)";

// The enclosing type's nested `C` — the member §2.7's chain-head row ranks
// LAST — whose nested `N` returns 6.
constexpr const char* kEnclosing = R"(
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

// True iff §2.7's warning row fired for the chain head `C`: a chain head
// resolving to a type while a same-named accessible member exists on the
// enclosing type.
//
// fix #7085 (round 2): in the TYPE position the shadowed member is always a
// NESTED TYPE (a type body scope registers only the type's generic
// parameters and its nested types), so the row names the member kind
// "nested type" — the VALUE position's "same-named member" wording belongs
// to the value-member case, whose text is byte-identical to its pre-#7085
// text.
template <class Diags>
bool hasChainHeadShadowWarning(const Diags& diags) {
    for (const auto& d : diags.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("chain head `C`") != std::string::npos &&
            d.message.find("same-named nested type") != std::string::npos)
            return true;
    return false;
}

// True iff a diagnostic carries `text` — the exact-text assertion for the
// warning's REMEDY, which is the point of fix #7085's round 2: `this.C`
// does not parse in a type position, so the row must name the ENCLOSING
// TYPE's name (`Outer.C`).
template <class Diags>
bool hasDiagnosticWith(const Diags& diags, const std::string& text) {
    for (const auto& d : diags.diagnostics())
        if (d.message.find(text) != std::string::npos) return true;
    return false;
}

// ============================================================================
// Part 1 — the defect. The imported type outranks the enclosing member.
// ============================================================================

// §2.7 (v0.343): the issue's literal repro. The chain head `C` resolves to
// the IMPORTED type, so the program prints 7 — on the base it printed 6 (the
// enclosing `Outer.C` won, which §2.7's chain-head row forbids).
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           imported_type_outranks_enclosing_member_at_a_chain_head_spec_2_7) {
    const std::string app = std::string("module App;\nimport LC;\n") +
                            kEnclosing;
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App", app}},
                                "App");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// ============================================================================
// Part 2 — CONTROLS. The fix must not change any of these.
// ============================================================================

// §2.7 (v0.343): with NO import, nothing outranks the enclosing type's
// nested `C`, so it is still the chain head's referent and the program
// prints 6.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           enclosing_member_still_wins_when_nothing_else_has_the_name_spec_2_7) {
    const std::string app = std::string("module App3;\n") + kEnclosing;
    JitResult r = runAotModules({ModuleSource{"App3", app}}, "App3");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// §2.2 (v0.343): the import stays reachable by QUALIFIER — "A declaration
// in the current module shadows any same-named import for bare-name purposes
// (the import stays reachable by qualifier)". `LC.C.N` is that qualified
// spelling, and the fix does not touch it.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           module_qualified_spelling_still_reaches_the_import_spec_2_2) {
    const std::string app = R"(
module App4;
import LC;
class Outer
  public class C
    public class N
      public method Get(): int return 6; end Get;
    end N;
  end C;
  public class Inner
    public static method Main(): int
      using var n: LC.C.N := new LC.C.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Outer;
)";
    JitResult r = runAotModules({ModuleSource{"LC", kLib},
                                 ModuleSource{"App4", app}},
                                "App4");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §2.7 rule 1 (v0.343): the CURRENT MODULE's declarations come before the
// imported bare names, so a module-level `C` still wins over the import —
// the chain-head row moves the ENCLOSING type's members, not the module's
// own declarations. The program prints 4 (the module-level `C`'s nested
// `N`).
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           module_level_declaration_still_outranks_the_import_spec_2_2) {
    const std::string app = R"(
module App5;
import LC;
class C
  public class N
    public method Get(): int return 4; end Get;
  end N;
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
                                 ModuleSource{"App5", app}},
                                "App5");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// ============================================================================
// Part 3 — §2.7's WARNING ROW. The same rule's other half.
//
// The warning is asserted through the SEMA-ONLY oracle: the AOT oracle
// cannot see a warning (see ORACLES above). The shape is the import-free
// twin of the defect — a MODULE-LEVEL type beside the enclosing type's
// same-named nested type — which is the same rule: the chain head resolves
// to a type while a same-named accessible member is reachable.
// ============================================================================

// §2.7's warning row (v0.343): the chain head resolves to a type while the
// enclosing type's same-named member is still reachable, so the row polices
// it. The carve-out does not apply: the member's declared type is the
// nested type `Outer.C`, not the resolved type `C`.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           chain_head_shadowing_the_enclosing_member_warns_spec_2_7) {
    const std::string src = R"(
module Warn1;
public class C
  public class N
    public method Get(): int return 4; end Get;
  end N;
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
    auto r = bezel::test::parseAndResolveGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasChainHeadShadowWarning(r.diagnostics));
}

// §2.7's warning row (v0.343): the row's CARVE-OUT — "warn, unless the
// member's declared type is the resolved type (or an instantiation of the
// resolved generic)". With nothing else answering the name, the chain head
// resolves to the enclosing type's own nested type — the member IS the
// resolved type — so the row does not fire.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           no_warning_when_the_chain_head_resolves_to_the_member_itself_spec_2_7) {
    const std::string src = R"(
module Warn2;
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
    auto r = bezel::test::parseAndResolveGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasChainHeadShadowWarning(r.diagnostics));
}

// fix #7085 (round 2): the warning's REMEDY in the TYPE position. §2.7 says
// "A member shadowed at a chain head stays reachable by qualifying
// (`this.Data.W()`; the enclosing type's name for statics)" — and in a TYPE
// position the shadowed member is a NESTED TYPE, whose reachable spelling is
// the ENCLOSING TYPE's name: `Outer.C`. `this.C` does not even parse in a
// type position (MEASURED: "expected type name"), so a warning that advised
// it advised a program that cannot compile.
//
// The row pins the remedy's TEXT, not just the warning's presence: the
// presence is pinned by the row above, and the remedy is the part that was
// wrong.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           chain_head_warning_names_the_enclosing_type_remedy_spec_2_7) {
    const std::string src = R"(
module Warn4;
public class C
  public class N
    public method Get(): int return 4; end Get;
  end N;
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
    auto r = bezel::test::parseAndResolveGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticWith(
        r.diagnostics,
        "while the enclosing type declares a same-named nested type"));
    BEZEL_EXPECT_TRUE(
        hasDiagnosticWith(r.diagnostics,
                          "qualify with `Outer.C` to reach the nested type"));
    // The `this.` advice is the VALUE position's, and it is wrong in a type
    // position: the row must not offer it.
    BEZEL_EXPECT_FALSE(
        hasDiagnosticWith(r.diagnostics, "qualify with `this.C`"));
}

// ============================================================================
// Part 4 — §11.6: the enclosing type's TYPE PARAMETER is not a member.
//
// §2.7's chain-head row ranks "the enclosing type's accessible MEMBERS"
// last. A type PARAMETER is not a member: §11.6 puts it out of scope in a
// nested type ENTIRELY ("The enclosing type's type parameters are therefore
// not in scope in a nested type's declaration … A reference to one is a
// compile error naming this rule"). So a type-parameter binding must not be
// deferred as if it were a member: deferring it let the name resolve
// OUTWARD (to a module-level or imported type of the same name) and deleted
// §11.6's compile error.
//
// These rows PIN that compile error, with the diagnostic §11.6/§14.1 names.
// ============================================================================

// §11.6 / §14.1 (v0.343): a nested type that references the enclosing type's
// type parameter is a compile error — even when the CURRENT MODULE declares
// a type of the same name, which the deferred lookup used to resolve to.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           nested_type_may_not_use_the_enclosing_type_parameter_when_a_module_type_has_the_name_spec_11_6) {
    const std::string src = R"(
module App3;
class T
  public class N
    public method Get(): int return 9; end Get;
  end N;
end T;
class Box<T>
  public class Inner
    public static method Main(): int
      using var n: T.N := new T.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Box;
)";
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not use the enclosing type's type parameters"));
}

// §11.6 / §14.1 (v0.343): the same, with an IMPORTED type of the same name
// — the imported bare name is a rule-1 position above the enclosing type's
// members, but a type PARAMETER is not a member and is not a candidate at
// all: §11.6's compile error stands.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           nested_type_may_not_use_the_enclosing_type_parameter_when_an_imported_type_has_the_name_spec_11_6) {
    const std::string lib = R"(
module LM2;
public class T
  public class N
    public method Get(): int return 7; end Get;
  end N;
end T;
)";
    const std::string app = R"(
module App5b;
import LM2;
class Box<T>
  public class Inner
    public static method Main(): int
      using var n: T.N := new T.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Box;
)";
    JitResult r = runAotModules({ModuleSource{"LM2", lib},
                                 ModuleSource{"App5b", app}},
                                "App5b");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not use the enclosing type's type parameters"));
}

// §11.6 / §14.1 (v0.343): nothing else holds the name — the §11.6 guard's
// own row. It stays pinned, not merely still passing.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           nested_type_may_not_use_the_enclosing_type_parameter_spec_11_6) {
    const std::string src = R"(
module App3b;
class Box<T>
  public class Inner
    public static method Main(): int
      using var n: T.N := new T.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Box;
)";
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not use the enclosing type's type parameters"));
}

// §11.6 / §14.1 (v0.343): the #3584-class SOUNDNESS guard — a nested
// type's FIELD typed with the enclosing type's type parameter. Left
// undiagnosed, the field is lowered to an unspecialized layout and every
// instantiation shares one struct.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           nested_type_field_may_not_use_the_enclosing_type_parameter_spec_11_6) {
    const std::string src = R"(
module App3c;
class Box<T>
  public class Inner
    private _v: T;
    public method Get(): T return _v; end Get;
  end Inner;
  public static method Main(): int
    return 0;
  end Main;
end Box;
)";
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not use the enclosing type's type parameters"));
}

// §11.6 + §2.7's chain-head row (v0.343): a nested type of the CURRENT type
// that carries the enclosing type's parameter's NAME is still the current
// type's own member — §11.6 puts the enclosing type's PARAMETER out of
// scope in a nested type, so the name belongs to the nested type (and to
// §2.7's chain-head row, which ranks that member last — nothing else here
// holds the name, so it wins). The program prints 6.
//
// MEASURED: the base compiled this and printed 6; deferring the parameter
// as if it were a member rejected it with §11.6's diagnostic, which names a
// reference the program does not make.
BEZEL_TEST(chain_head_import_outranks_enclosing_member_7085,
           nested_type_named_like_the_enclosing_type_parameter_still_wins_spec_11_6) {
    const std::string src = R"(
module AppMix;
class Box<T>
  public class Inner
    public class T
      public class N
        public method Get(): int return 6; end Get;
      end N;
    end T;
    public static method Main(): int
      using var n: T.N := new T.N();
      Console.WriteLine(n.Get());
      return 0;
    end Main;
  end Inner;
end Box;
)";
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

}  // namespace

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
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
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
template <class Diags>
bool hasChainHeadShadowWarning(const Diags& diags) {
    for (const auto& d : diags.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("chain head `C`") != std::string::npos &&
            d.message.find("same-named member") != std::string::npos)
            return true;
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

}  // namespace

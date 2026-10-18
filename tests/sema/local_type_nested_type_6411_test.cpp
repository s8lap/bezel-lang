// Regression test for §11.3's nested-type REGISTRATION on a §21.10 LOCAL type
// (sema, v0.338).
//
// THE GOVERNING RULE. §11.3 is what makes a nested type a member of the type
// that declares it, and the registration that does it is a single loop:
// `resolveTypeDecl`'s §11.3/#482 loop walks `td.members`, finds each
// `NestedTypeMember`, and stores its real `NestedTypeSymbol*` under the nested
// type's name in that type's OWN body scope — before the member walk, so the
// body is order-independent.
//
// §21.10 does not exempt a local type from any of that. §21.10 "Local type
// declarations" is explicit that a local type is an ordinary nominal record or
// enum: "The prohibition does NOT reach the local type's OWN members: the local
// type is an ordinary nominal record or enum whose members are declared by the
// §9/§10 rules UNCHANGED … and the name IS in scope inside its own body", and
// "Scope and name discipline" puts "the static members AND NESTED TYPES of the
// type whose body declares it" in the chain a bare name reaches there.
//
// THE DEFECT (#6411). §11.3's loop lives in `resolveTypeDecl`, and a §21.10
// local type NEVER ENTERS `resolveTypeDecl` — `resolveNamed`'s §21.10 arm
// collects its `TypeSymbol` through `collectTypeDecl` and walks its members
// itself. So a nested type declared inside a local record was entered into NO
// symbol table at all. Its name neither RESOLVED nor BLOCKED inside the local
// record's body, and a bare spelling of it bound the MODULE-LEVEL type of the
// same name instead.
//
// THE FAILURE MODE IS A CONFUSING DIAGNOSTIC, which is why this is worth its
// own issue rather than a comment. The program points at the nested type's own
// name and reports a missing member ON THE MODULE-LEVEL TYPE — a declaration the
// user never wrote and never referred to — which sends the reader to the wrong
// type entirely. The worse half needs no missing member at all: when the
// module-level type happens to carry the member being read, the program
// COMPILES CLEAN on the WRONG type. Row 3 is that spelling and it is the row
// that matters; row 2 is the loud one.
//
// MEASURED on the base (bezelc built in this lane worktree at 863d6c6b, with
// #6410's fix already applied): row 1 REJECTED with "no member `W` on type
// `Box` (§11.3, §12.1)"; row 2 COMPILED CLEAN AND RAN, binding `Box` to the
// module-level `record Box(V: int)` and reading `V` off it — a SILENT
// ACCEPTANCE of a program that means `Inner.Box`. Row 3 (the same program with
// the nested type declared BELOW the member that names it) rejected the same
// way, and is here to pin that the fix runs the loop BEFORE the member walk
// rather than after it. Row 5 has no module-level `Box` at all and rejected as
// a bare unresolved name. Row 4 is the §11.6 nested twin, which already worked.
//
// Rows 1, 2, 3, 5 and 6 are the five that are RED on the base and green here
// (measured on the reverted source). Row 4 is green on both and is the control
// that says the rule itself did not move.
//
// THE FIX is the §11.3 loop, at its second site. It runs in `resolveNamed`'s
// §21.10 arm, before that arm's member walk, and stores the real
// `NestedTypeSymbol*` — not the nullptr generic-parameter marker, which
// `resolveNamed` reads as "generic param" with `declaration = nullptr` and so
// gives an unspecialized layout shared by every instantiation (#3584's class of
// defect). It is a registration added and nothing else: §2.7's stop reads the
// paired `TypeSymbol`, not this scope, and a nested type admitted by bare name
// is an ANSWER that short-circuits the walk rather than a blocker, so no
// rejection is added anywhere by this change.
//
// ORACLE. runAot (the real AOT path) on every row. Each accept row asserts
// `r.ran` as well as the absence of a compile-time failure, so no
// skip-absorbing guard can stand in for the program.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Every row's program ends with the same read-back, so the only thing that
// varies between rows is which `Box` the bare name is meant to find. The
// MODULE-level declaration is `record Box(V: int)` — on a DIFFERENT shape from
// the local `record Box(W: int)` — precisely so a wrong binding is observable
// rather than silent.

// ── 1. THE LOCAL TYPE'S NESTED TYPE RESOLVES BY BARE NAME ───────────────
//
// `Get(): Box` names `Inner`'s OWN nested `Box`, and `i.Get().W` reads the
// field that only `Inner.Box` has. Both halves fail on the base.
BEZEL_TEST(local_type_nested_type_6411,
           spec_11_3_local_types_nested_type_resolves_by_bare_name) {
    const std::string src =
        "module M; import System; public record Box(V: int); "
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "  public method Get(): Box return new Box(7); end Get; "
        "end Inner; "
        "var i := new Inner(); return i.Get().W; "
        "end M; end Outer; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 2. AND THE WRONG BINDING IS NOT MERELY LOUD, IT IS MISLEADING ───────
//
// Row 1's discriminator is `W`, a field of `Inner.Box` alone. This row drops
// `W` and reads `V` instead — a field of the MODULE-LEVEL `Box` alone — so the
// two shapes are mirror images and only the correct binding can satisfy the
// right one. On the base this program COMPILED CLEAN: `Box` bound to the
// module-level declaration and `V` read off it, which is the silent acceptance
// this fix removes. It is asserted as a REJECT with the missing-member
// diagnostic, because after the fix the only `Box` in scope inside `Inner` is
// `Inner.Box`, which has no `V`.
BEZEL_TEST(local_type_nested_type_6411,
           spec_11_3_local_types_nested_type_never_binds_the_module_type) {
    const std::string src =
        "module M; import System; public record Box(V: int); "
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "  public method Get(): Box return new Box(7); end Get; "
        "end Inner; "
        "var i := new Inner(); return i.Get().V; "
        "end M; end Outer; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── 3. ORDER-INDEPENDENCE: THE NESTED TYPE MAY BE DECLARED LATER ────────
//
// `resolveTypeDecl` runs its §11.3 loop BEFORE the member walk, so a §11.6
// nested type is visible to a member declared above it. A fix that ran the
// loop after the walk would satisfy rows 1 and 2 — both of which declare the
// nested type FIRST — and leave this spelling binding the module-level type
// again. So the axis that distinguishes them is declaration order alone.
BEZEL_TEST(local_type_nested_type_6411,
           spec_11_3_local_nested_type_declared_after_the_member_still_resolves) {
    const std::string src =
        "module M; import System; public record Box(V: int); "
        "public class Outer public method M(): int "
        "record Inner "
        "  public method Get(): Box return new Box(7); end Get; "
        "  public record Box(W: int); "
        "end Inner; "
        "var i := new Inner(); return i.Get().W; "
        "end M; end Outer; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 4. THE §11.6 TWIN ALREADY WORKED, AND MUST KEEP WORKING ─────────────
//
// The control that says rows 1–3 are about the LOCAL-type path and not about
// the rule. A §11.6 nested type goes through `resolveTypeDecl` and was
// registered there all along, so this program compiled clean on the base and
// must compile clean now — a fix that registered local types by some route
// that disturbed the ordinary path would break it.
BEZEL_TEST(local_type_nested_type_6411,
           spec_11_3_nested_twins_nested_type_in_a_nested_type_still_resolves) {
    const std::string src =
        "module M; import System; public record Box(V: int); "
        "public class Outer public record Inner "
        "  public record Box(W: int); "
        "  public method Get(): Box return new Box(7); end Get; "
        "end Inner; end Outer; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 5. A LOCAL TYPE'S NESTED TYPE IS STILL A NESTED TYPE, NOT A MEMBER ─
//
// §2.7 admits "a static member OR A NESTED TYPE" of an enclosing type by bare
// name, and §2.7's stop sentence is about a member that CANNOT BE USED in a
// type position. A nested type is usable in one, so registering it must not
// turn it into a blocker: a local `record Inner` carrying `public record Box(W:
// int);` still resolves a bare `Box` inside its own body.
//
// This row is red on the base for the REGISTRATION reason (with no module-level
// `Box` in the program at all, the unregistered name resolved to nothing), so it
// is one of the four that prove the fix. What it pins on top of rows 1-3 is
// the direction: a registration that put the name where §2.7's stop reads it —
// on the paired `TypeSymbol` rather than in the body scope — would reject this
// program, and the newly-accepted set would then include only rows 1 and 3 by
// accident rather than by the rule. There is NO module-level `Box` here on
// purpose: without one, "the search found nothing" and "the stop ended it" are
// the only two ways this can fail, and this row says it is the first.
BEZEL_TEST(local_type_nested_type_6411,
           spec_2_7_local_nested_type_is_an_answer_not_a_blocker) {
    const std::string src =
        "module M; import System; "
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "  public method Make(): Box return new Box(7); end Make; "
        "end Inner; "
        "var i := new Inner(); return i.Make().W; "
        "end M; end Outer; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 6. THE NEAREST LINK WINS: THE LOCAL TYPE'S NESTED TYPE OVER AN OUTER ─
//
// Rows 1-5 have no member of the colliding name anywhere else, so they cannot
// distinguish "the nested type is now the answer" from "the search no longer
// stops anywhere". This row can, and the answer is the one §2.7 gives.
//
// `Holder` — the type whose body declares the local `Inner` — declares
// `method Box(): int`. §2.7's chain is "the nested type and then each type
// enclosing it, innermost first … the first type that HAS AN ACCESSIBLE MEMBER
// OF THAT NAME ENDS THE SEARCH". `Inner` is the FIRST such type: it has an
// accessible member named `Box`, and that member is a NESTED TYPE — which §2.7
// admits by bare name ("Nested types of an enclosing type, generic or not, are
// reached by bare name, since a nested type is one type and needs no
// instantiation"). So the search ends at `Inner` with `Inner.Box` as the
// ANSWER, and `Holder`'s method is never reached.
//
// §21.10 puts that chain in scope explicitly for a local type ("a bare name
// reaches the static members and nested types of the type whose body declares
// it, and of that type's enclosing types"), so the local type's OWN nested type
// is the nearest link and outranks the declaring type's member. The control is
// row 28 of `enclosing_member_bare_type_name_test.cpp`
// (`spec_2_7_enclosing_toplevel_member_stops_a_local_types_search`): the same
// program with `Inner`'s nested type deleted still stops at `Holder`.
BEZEL_TEST(local_type_nested_type_6411,
           spec_2_7_local_nested_type_outranks_the_declaring_types_member) {
    const std::string src =
        "module M; import System; "
        "public class Holder "
        "  public method Box(): int return 1; end Box; "
        "  public method M(): int "
        "    record Inner "
        "      public record Box(W: int); "
        "      public method Get(): Box return new Box(7); end Get; "
        "    end Inner; "
        "    var i := new Inner(); return i.Get().W; "
        "  end M; "
        "end Holder; "
        "public class Program public static method Main() "
        "Console.WriteLine(\"x\"); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// Spec-derived tests for fix #4167 — §27.8 "Type aliases" / "Generic type
// aliases" (v0.298).
//
// #4144 (commit 6095982) closed §26.2's extendable-target list against four
// of its five forbidden kinds, deliberately leaving "type parameters"
// (`extension T` is meaningless) uncovered: its own rationale argued no live
// program can produce a TypeRef shape representing "the target IS a type
// parameter" that the pre-existing "unresolved type name" fallback does not
// already catch.
//
// #4167 refutes that rationale: referencing a GENERIC type alias WITHOUT its
// required type-argument list resolves to exactly that shape.
// `resolveTopDecl`'s `TypeAlias` arm (TypeResolution_Names.cpp) resolves a
// generic alias's own body with the alias's OWN generic parameters bound as
// `nullptr` markers in a throwaway scope. For `type IdP11<T> := T;`,
// resolving the body `T` hits `resolveNamed`'s "is this name a generic-param
// marker?" branch, producing `TypeRef{Kind::Named, declaration=nullptr}` —
// and THIS is the TypeRef the alias's own resolveNamed's `TypeAliasSymbol`
// branch (the alias-transparency copy, fix #578) hands to every later
// reference to `IdP11`, regardless of what — if anything — that reference
// itself supplies as type arguments: the branch never substituted
// `t.genericArgs` into the cached ref at all. So `extension IdP11` (bare, no
// `<...>`) reached `rejectDisallowedExtensionTarget`'s `Named` arm with
// `declaration == nullptr` — the exact shape #4144 reasoned could not occur
// outside its own already-covered "unresolved name" path — and that arm
// only inspects `declaration` when it is non-null (a null `declaration` is
// *usually* legitimate: every locally-resolved built-in without a user
// TypeSymbol, `Array<T>`/`IEnumerable<T>`/etc., shares that same shape), so
// the declaration compiled with zero diagnostics and permanently dead,
// uncallable members.
//
// Root-cause fix (not the extension-local patch #4167 sketched as its
// narrower "direction 2"): #4167's own write-up offered two directions and
// investigation found direction 2 unsound as stated — "reject a Named ref
// with declaration==nullptr unless importedName is set" would ALSO reject
// every legitimate LOCAL extension of a declaration-less builtin
// (`extension Array<int>` and friends all resolve with declaration=nullptr
// and an empty importedName — that field is populated only for a TypeRef
// reconstructed from cross-module metadata, per TypeRef.hpp's own comment,
// "Always empty for locally-resolved TypeRefs"). Direction 1 — a general
// arity check on generic-alias references, added once, where the
// alias-transparency branch resolves a `TypeAliasSymbol` (resolveNamed,
// TypeResolution_Names.cpp) — closes the hole at its root, for every
// reference context a bare/miscounted generic-alias name can appear in
// (locals, fields, parameters, extension targets, …), the same way
// §11.3/Appendix C.4's existing arity check already covers ordinary
// TypeSymbol references a few lines below in the same function. Unlike that
// check (which tolerates an entirely omitted argument list — a "raw"
// reference to a real declared type, still carrying a genuine declaration),
// a generic alias has no safe fallback: its cached underlying ref IS the
// only representation available, and is indistinguishable from a bare
// type-parameter reference whenever the alias's body reduces to one. So the
// new check rejects a bare (zero-argument) reference too, whenever the
// alias itself is generic.
//
// Test oracle choice: every rejection row uses
// `failedAtCompileTimeWithRealLocation` (excludes a crash/signal/timeout/
// ASan outcome) AND a diagnostic-text check naming the new §27.8 wording, so
// a row can only pass if the compiler produced exactly the intended clean
// rejection, not a crash whose backstop diagnostic happens to carry a real
// location too (§18 of the lane process).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// REJECT: a generic type alias referenced with the wrong number of type
// arguments (including the "wrong number" of zero).
// ---------------------------------------------------------------------------

// §26.2 / §27.8 — the issue's own repro, verbatim: an identity generic
// alias (`type IdP11<T> := T;`) referenced BARE as an extension target.
// Pre-fix: compiles with zero diagnostics, permanently dead members.
// Post-fix: rejected at the alias-reference site with a real location.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           bare_identity_alias_extension_target_rejected) {
    auto r = runAot(
        "module M; "
        "type IdP11_4167a<T> := T; "
        "extension IdP11_4167a "
        "  public method Foo4167(): string => \"id\"; "
        "end; "
        "public static class Program4167a "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type alias"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// §27.8 — the same identity alias, but with the WRONG NUMBER of arguments
// supplied (2 where the alias declares 1), not merely zero. Proves the
// check is a real arity comparison, not just a zero-args special case.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           wrong_arity_identity_alias_extension_target_rejected) {
    auto r = runAot(
        "module M; "
        "type IdP11_4167b<T> := T; "
        "extension IdP11_4167b<int, string> "
        "  public method Foo4167(): string => \"id\"; "
        "end; "
        "public static class Program4167b "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type alias"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// §27.8 — the fix is NOT extension-local: a bare reference to a generic
// alias is rejected in an ordinary local-variable type position too, the
// same way it is at an extension target. Uses a NON-identity alias
// (`Lookup4167c<K, V> := Dictionary<K, Array<V>>`) to show the check is not
// specific to the identity-collapse shape either — this is the class of
// bug #4167's own "not yet checked" paragraph flagged as a separate,
// deeper concern for a non-identity alias; the general arity check closes
// it too, for free.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           bare_nonidentity_generic_alias_local_var_rejected) {
    auto r = runAot(
        "module M; "
        "type Lookup4167c<K, V> := Dictionary<K, Array<V>>; "
        "public static class Program4167c "
        "  public static method Main(): int "
        "    var x: Lookup4167c; "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type alias"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "27.8"));
}

// ---------------------------------------------------------------------------
// ACCEPT: every legitimate extension target must keep compiling. The base
// target kinds (class/interface/record/enum/primitive/generic-instantiation)
// are unaffected by construction — this fix touches only the TypeAliasSymbol
// branch of resolveNamed — and are already pinned by
// tests/sema/extension_target_kind_gate_4144_test.cpp; the rows below cover
// what IS newly reachable through this fix's code path: aliases (to every
// kind), an alias chain, and a generic identity alias instantiated with the
// CORRECT arity.
// ---------------------------------------------------------------------------

// A non-generic alias to a class must still be extendable, and the
// extension member must still be callable — the arity check must not
// misfire on the overwhelmingly common (non-generic) alias shape (0
// declared params, 0 supplied args).
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           alias_to_class_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public class Widget4167d "
        "  public property Name: string := \"w\"; "
        "end Widget4167d; "
        "type WidgetAlias4167d := Widget4167d; "
        "extension WidgetAlias4167d "
        "  public method Shout4167(): string => this.Name + \"!\"; "
        "end; "
        "public static class Program4167d "
        "  public static method Main(): int "
        "    var w := new Widget4167d(); "
        "    Console.WriteLine(w.Shout4167()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "w!\n");
}

// A non-generic alias to an interface must still be extendable and callable
// through a concrete implementer.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           alias_to_interface_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public interface IThing4167e "
        "  method Bar4167(): int; "
        "end IThing4167e; "
        "public class Impl4167e: IThing4167e "
        "  public method Bar4167(): int => 9; "
        "end Impl4167e; "
        "type IThingAlias4167e := IThing4167e; "
        "extension IThingAlias4167e "
        "  public method Describe4167(): int => this.Bar4167() + 1; "
        "end; "
        "public static class Program4167e "
        "  public static method Main(): int "
        "    var t: IThing4167e := new Impl4167e(); "
        "    return t.Describe4167(); "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// A non-generic alias to a record must still be extendable and callable.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           alias_to_record_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public record Pt4167f(X: int, Y: int); "
        "type PtAlias4167f := Pt4167f; "
        "extension PtAlias4167f "
        "  public method Sum4167(): int => this.X + this.Y; "
        "end; "
        "public static class Program4167f "
        "  public static method Main(): int "
        "    var p := new Pt4167f(3, 4); "
        "    return p.Sum4167(); "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// A non-generic alias to an enum must still be extendable and callable.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           alias_to_enum_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public enum Color4167g "
        "  Red, Green, Blue; "
        "end Color4167g; "
        "type ColorAlias4167g := Color4167g; "
        "extension ColorAlias4167g "
        "  public method Ordinal4167(): int => (case this of "
        "    Color4167g.Red => 0, "
        "    Color4167g.Green => 1, "
        "    Color4167g.Blue => 2, "
        "  ); "
        "end; "
        "public static class Program4167g "
        "  public static method Main(): int "
        "    var c := Color4167g.Green; "
        "    return c.Ordinal4167(); "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// An alias CHAIN (A := B; B := SomeClass) must still resolve and extend
// through both hops.
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           alias_chain_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public class Base4167h "
        "  public property N: int := 5; "
        "end Base4167h; "
        "type ChainB4167h := Base4167h; "
        "type ChainA4167h := ChainB4167h; "
        "extension ChainA4167h "
        "  public method Doubled4167(): int => this.N * 2; "
        "end; "
        "public static class Program4167h "
        "  public static method Main(): int "
        "    var b := new Base4167h(); "
        "    return b.Doubled4167(); "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// A generic IDENTITY alias instantiated with the CORRECT arity
// (`IdP11<int>`, matching the alias's own single declared parameter) must
// keep compiling — this is the exact shape the issue names as "legal and
// must not be caught by whatever you add". NOTE: the declaration compiling
// is all this row asserts. Generic type-alias ARGUMENT SUBSTITUTION is a
// separate, pre-existing, unimplemented mechanism (the alias-transparency
// branch never binds `t.genericArgs` into the cached underlying ref, arity
// match or not) — filed separately (see #4169) — so a real call through a
// concrete receiver does not dispatch to this extension member yet; that is
// not a new regression from this fix; it reproduces identically before and
// after (measured).
BEZEL_TEST(extension_generic_alias_target_arity_4167,
           identity_alias_instantiated_concretely_extension_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "type IdP11_4167i<T> := T; "
        "extension IdP11_4167i<int> "
        "  public method Foo4167(): string => \"id\"; "
        "end; "
        "public static class Program4167i "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

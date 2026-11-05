// fix #8213 (§27.8, v0.355): a type alias's body is resolved in the scope of
// its DECLARATION, not in the scope of each REFERENCE site.
//
// THE DEFECT. §27.8: "A type alias is a pure synonym — it does not create a
// new type. `UserId` and `int` are interchangeable." The alias names the ONE
// type its declaration names, and the names its body may name are the ones
// visible AT THAT DECLARATION — the module scope (an alias is a module-level
// declaration, A.2 `TypeAliasDecl` is a `TopDecl`) plus the alias's own
// generic parameters.
//
// `Resolver::resolveNamed`'s type-alias arm did neither. It resolved the body
// through a scope whose PARENT was the REFERENCE site's scope:
//
//     Scope aliasScope;
//     aliasScope.parent = scope;          // the REFERENCE site's scope
//     for (const auto& g : aliasDecl->genericParams)
//         aliasScope.names[g.name] = nullptr;   // the alias's OWN params
//
// so a body that spells a reserved built-in was captured by a type parameter
// of the same spelling at the reference site:
//
//     type GMap := Dictionary<Guid, int>;      // module scope: Guid = System.Guid
//     public class U
//       public static method F<Guid>(d: GMap): int   // F's own `Guid` param
//         return d.Count;
//       end F;
//     end U;
//
// Inside `F<Guid>` the alias denoted `Dictionary<F's Guid, int>` — NOT
// `Dictionary<System.Guid, int>`. The alias therefore denoted a DIFFERENT type
// at each reference site, which is exactly what §27.8's "pure synonym" forbids,
// and it produced wrong verdicts in BOTH directions:
//
//   * r4c: `F<int>(d)` with a `Dictionary<int, int>` was ACCEPTED (the
//     captured body made the parameter's type `Dictionary<int, int>`);
//   * r4e: `F<int>(d)` with a real `Dictionary<Guid, int>` was REJECTED (the
//     parameter's type was `Dictionary<int, int>`, the argument was not);
//   * r4b: `F<Guid>(d: GMap)`'s body `d[0]` — an `int` index on a
//     Guid-keyed dictionary — was not caught, because the key type was F's
//     abstract type parameter.
//
// The capture had TWO channels, and both had to close:
//
//   (1) the SCOPE the body is resolved in (`aliasScope.parent`), fixed here
//       by resolving the body in the alias's DECLARATION scope — the same
//       scope object `resolveTopDecl`'s `TypeAlias` arm and
//       `resolveAliasForwardReferences` already use for it, so a body now
//       resolves IDENTICALLY at its declaration and at every reference site;
//   (2) the SUBSTITUTION of an enclosing generic's type parameters into the
//       alias-expanded type. The alias's expansion is a CONCRETE type
//       (`Dictionary<System.Guid, int>`), so an enclosing method's parameter
//       named `Guid` must not rewrite the body's `Guid` — but the name-keyed
//       substitution walkers did. The fix gives those walkers the RESOLVER'S
//       OWN record of what each spelling resolved to (the tri-state
//       `TypeRef::typeParam`, #6530's per-TypeRef record), so a spelling the
//       resolver resolved to a built-in is not substituted by name.
//
// §14.1 is the source of the answer for (2): "is this name a TYPE PARAMETER?"
// is a question about the SCOPE, not about the spelling. The resolver already
// answers it per resolution; the substitution now asks it.
//
// Every row below is on the `runAot` oracle. The probe programs are the
// issue's own repros, verbatim (r4b/r4c/r4e from the #6530d lane's probe
// directory; r4f/r4g from the issue's review comment).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The rejection oracle: `r.ran == false` ALONE is satisfied by a crash, a
// signal, a timeout and an ASan report as well as by a genuine rejection.
// What discriminates is the WORDING plus a real source location.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// The alias's body, resolved in its DECLARATION scope, names the built-in
// `Guid` — so the index argument of `d[0]` is an `int` where the key type
// `System.Guid` is required, and §5.1 has no implicit `int` -> `Guid`
// conversion (§3.6 rule 2, §5.1, §14.2).
constexpr const char* kIndexNotConvertible =
    "the index argument to `Dictionary[key]` has a type that is not "
    "implicitly convertible to the target type (\xC2\xA7""5.1)";

// The call-site argument check: `Dictionary<int, int>` is not a
// `Dictionary<System.Guid, int>` (§5.1).
constexpr const char* kArgumentNotConvertible =
    "the argument `d` to call to `F` has a type that is not implicitly "
    "convertible to the target type (\xC2\xA7""5.1)";

}  // namespace

// ── r4b: the alias body is the built-in, so `d[0]` is an `int` index on a
//        Guid-keyed dictionary and is rejected ───────────────────────────────
//
// The issue's own repro, verbatim. The method `F` is never CALLED, so the
// rejection must come from the DECLARATION — the body's own type — and not
// from any call site.
//
// Pre-fix: ACCEPTED, and printed "ok" (the alias body's `Guid` was captured
// by `F`'s type parameter, so the index argument was checked against an
// abstract key type and no §5.1 diagnostic fired).
BEZEL_TEST(type_alias_declaration_scope_8213,
           r4b_alias_body_builtin_is_not_captured_by_the_reference_site) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d[0];\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIndexNotConvertible));
}

// ── r4c: the capture, observed at the call site ───────────────────────────
//
// `F<int>` binds `F`'s type parameter `Guid` to `int`, and the argument is a
// `Dictionary<int, int>`. §27.8: the alias denotes `Dictionary<System.Guid,
// int>` at every reference site, so the argument is NOT convertible to it and
// §5.1 rejects the call.
//
// Pre-fix: ACCEPTED and printed 2 — the alias body's `Guid` was rewritten to
// the call site's `int`, so the parameter's type was `Dictionary<int, int>`
// and the mistyped argument was admitted.
BEZEL_TEST(type_alias_declaration_scope_8213,
           r4c_alias_body_builtin_is_not_captured_by_a_call_site_type_argument) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d.Count;\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var d: Dictionary<int, int> := [0: 1, 1: 2];\n"
        "    Console.WriteLine(U.F<int>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(rejectedWith(r, kArgumentNotConvertible));
}

// ── r4e: the mirror — a correctly typed argument is ACCEPTED ──────────────
//
// The argument IS the alias's type: `Dictionary<System.Guid, int>`. §27.8
// makes the alias and that type interchangeable, so the call is admitted and
// the program prints the dictionary's `Count`.
//
// Pre-fix: REJECTED — "the argument `d` to call to `F` has a type that is not
// implicitly convertible to the target type (§5.1)" — because the alias body
// had been captured and the parameter's type had become
// `Dictionary<int, int>`. This row is the one that proves the fix did not
// over-reject: a synonym must still BE a synonym.
BEZEL_TEST(type_alias_declaration_scope_8213,
           r4e_alias_denotes_its_own_type_at_a_reference_site) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class Mk\n"
        "  public static method Make(): owned Dictionary<Guid, int>\n"
        "    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();\n"
        "    d[Guid.NewGuid()] := 1;\n"
        "    return d;\n"
        "  end Make;\n"
        "end Mk;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d.Count;\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var d := Mk.Make();\n"
        "    Console.WriteLine(U.F<int>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("1\n"), r.stdoutText);
}

// ── r4f: the same alias, called at the built-in — rejected at the index ───
//
// `F<Guid>` binds `F`'s type parameter to `Guid` and the argument IS the
// alias's type, so the CALL is fine; what the program does with the
// dictionary is not — `d[0]` is an `int` index on a `System.Guid`-keyed
// dictionary (§5.1 has no implicit `int` -> `Guid`).
//
// Pre-fix: ACCEPTED, and the program then threw
// `System.Collections.Generic.KeyNotFoundException` at run time (and, under
// ASan, a stack-buffer-overflow in `bz$valkey$hash` — #6536's mechanism).
// The rejection is a COMPILE-time one, so the row is safe under the forced
// ASan leg: nothing is instrumented, because nothing is generated.
BEZEL_TEST(type_alias_declaration_scope_8213,
           r4f_alias_called_at_the_builtin_is_rejected_before_it_runs) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d[0];\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var g := Guid.NewGuid();\n"
        "    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();\n"
        "    d[g] := 5;\n"
        "    Console.WriteLine(U.F<Guid>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(rejectedWith(r, kIndexNotConvertible));
}

// ── r4g: the alias, called with the captured type ─────────────────────────
//
// The mirror of r4f: `F<int>` with a `Dictionary<int, int>`. The alias's type
// is `Dictionary<System.Guid, int>`, so the argument is not convertible to it
// (§5.1) and the program's own `d[0]` is an `int` index on a Guid-keyed
// dictionary.
//
// Pre-fix: ACCEPTED and printed 7 — the alias body had been captured, the
// parameter's type was `Dictionary<int, int>`, and the mistyped argument was
// admitted.
BEZEL_TEST(type_alias_declaration_scope_8213,
           r4g_alias_called_with_the_captured_type_is_rejected) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d[0];\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var d: Dictionary<int, int> := [0: 7, 1: 2];\n"
        "    Console.WriteLine(U.F<int>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(rejectedWith(r, kArgumentNotConvertible));
}

// ── CONTROL: the alias denotes the SAME type at every reference site ──────
//
// Two methods, in the same module, naming the same alias — one whose own
// type parameter is spelled `Guid` and one whose is spelled `Z`. §27.8: an
// alias is a pure synonym, so BOTH denote `Dictionary<System.Guid, int>` and
// both admit a `Dictionary<Guid, int>` argument. This is the property the
// fix restores, stated over two reference sites at once: a fix that only
// closed one channel (the scope, but not the substitution) would still
// capture the body in the `Guid` method.
//
// Pre-fix: the `Guid` method REJECTED the correctly typed argument (the
// captured body made its parameter's type `Dictionary<Z's Guid, int>` at the
// call site, and the substitution rewrote the body's `Guid` to `int`).
BEZEL_TEST(type_alias_declaration_scope_8213,
           control_the_alias_denotes_one_type_at_every_reference_site) {
    auto r = runAot(
        "module P;\n"
        "type GMap := Dictionary<Guid, int>;\n"
        "public class U\n"
        "  public static method F<Guid>(d: GMap): int\n"
        "    return d.Count;\n"
        "  end F;\n"
        "  public static method G<Z>(d: GMap): int\n"
        "    return d.Count;\n"
        "  end G;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();\n"
        "    Console.WriteLine(U.F<int>(d));\n"
        "    Console.WriteLine(U.G<int>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("0\n0\n"), r.stdoutText);
}

// ── CONTROL: a GENERIC alias still binds its own type parameter ───────────
//
// The other side of §27.8: "each instantiation of the alias is
// interchangeable with the corresponding instantiation of the aliased type".
// `type Pair<T> := Dictionary<T, int>` binds its OWN `T`, and the enclosing
// method's `Z` — the parameter `d: Pair<Z>` — must still flow through the
// alias's expansion and be substituted at the call site. A fix that closed
// the substitution channel by NAME would break this row.
//
// The alias's own parameter is resolved in the alias's own scope, so this
// row is unaffected by the scope fix, and the `Z` in the expansion is a
// spelling the resolver resolved to a type PARAMETER, so the substitution
// channel still admits it.
BEZEL_TEST(type_alias_declaration_scope_8213,
           control_a_generic_alias_still_binds_its_own_type_parameter) {
    auto r = runAot(
        "module P;\n"
        "type Pair<T> := Dictionary<T, int>;\n"
        "public class U\n"
        "  public static method F<Z>(d: Pair<Z>): int\n"
        "    return d.Count;\n"
        "  end F;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var d: Dictionary<int, int> := [0: 1, 1: 2];\n"
        "    Console.WriteLine(U.F<int>(d));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string("2\n"), r.stdoutText);
}

// Spec-derived tests for fix #4144 — §26.2 "What can be extended" (v0.295).
//
// §26.2 states the extendable list is CLOSED:
//   admitted:    classes (including Object), interfaces, records, enums,
//                built-in primitive types.
//   NOT admitted: type parameters (`extension T` is meaningless), nullable
//                types directly (`extension int?`), function types,
//                choices, inline-array types (`extension [N]T`).
//
// Before this fix, NOTHING in src/sema/ gated `ExtensionDecl`'s resolved
// target kind against that list:
//   - `extension <Choice>` reached codegen and crashed with an unterminated
//     `case.end` basic block ("Basic Block ... does not have terminator!")
//     because an extension method's `MethodSymbol::parent` is permanently
//     null, so `choiceTypeOf` has no branch to recover the choice type for
//     an extension method's `case this of` body (the issue's own repro).
//   - `extension int?` (and any other top-level-nullable target) ALSO
//     crashed codegen, with a DIFFERENT signature ("Call parameter type
//     does not match function signature!") — `extensionTargetMatchesReceiver`
//     (ExprTypecheck.cpp) explicitly documents ignoring nullability
//     "because §26.2 forbids `extension int?`", an invariant nothing
//     enforced.
//   - `extension (int) -> int` (a function type) compiled and RAN, with the
//     extension member's body silently degraded to a no-op via the #3079
//     internal backstop (no crash, but not what the closed list permits).
//   - `extension [3]int` (an inline-array type) already had its call sites
//     rejected by an unrelated, pre-existing blanket member-lookup gate
//     (ExprTypecheck_Calls.cpp ~15859, citing §26.2) — but the DECLARATION
//     itself still compiled clean with dead, uncallable members.
//
// This fix adds `Resolver::rejectDisallowedExtensionTarget` (called from
// `resolveTopDecl`'s `ExtensionDecl` arm right after the target type
// resolves, TypeResolution_Names.cpp) to reject all four live-crash/
// live-gap kinds at the DECLARATION site, before any member of the block
// is typechecked further or reaches codegen.
//
// "Type parameters" (`extension T`) is the fifth listed-forbidden kind and
// is deliberately NOT covered by a dedicated reject here: an extension's
// own §26.4 type-parameter binding only auto-binds a bare identifier found
// in the target type's GENERIC ARGUMENT positions, never the target
// type's own head, so a bare `extension T` can only ever resolve `T` via
// ordinary name lookup — it either names a real declared type (legitimate,
// not an instance of this rule) or fails with the pre-existing, general
// "unresolved type name" diagnostic (`unresolved_type_param_repro_control`
// below pins that this path is unaffected).
//
// Test oracle choice: every rejection row uses
// `failedAtCompileTimeWithRealLocation` (tests/support/aot_result_asserts.hpp)
// — which explicitly EXCLUDES a crash/signal/timeout/ASan outcome — AND a
// diagnostic-text check naming the §26.2 wording, so a row can only pass if
// the compiler produced exactly the intended clean rejection. A bare
// `EXPECT_FALSE(r.ran)` would pass vacuously for the pre-fix CRASH too.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// REJECT: the four kinds §26.2 excludes that this fix newly gates.
// ---------------------------------------------------------------------------

// §26.2 "Choices" — the issue's own repro shape: extending a choice and
// pattern-matching `this` inside the extension method. Pre-fix: LLVM IR
// verifier crash ("Basic Block ... does not have terminator!"). Post-fix:
// clean §26.2 rejection at the extension's declaration site.
BEZEL_TEST(extension_target_kind_gate_4144, choice_target_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "public choice Shape4144 "
        "  Circle(radius: float), "
        "  Square(side: float); "
        "end Shape4144; "
        "extension Shape4144 "
        "  public method Describe(): string => (case this of "
        "    Circle(_) => \"circle\", "
        "    Square(_) => \"square\", "
        "  ); "
        "end; "
        "public static class Program4144a "
        "  public static method Main(): int "
        "    var s: Shape4144 := Shape4144.Circle(1.0); "
        "    Console.WriteLine(s.Describe()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot extend a choice type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "26.2"));
}

// §26.2 "Nullable types directly" — `extension int?`. Pre-fix: a DIFFERENT
// LLVM IR verifier crash ("Call parameter type does not match function
// signature!"), because `extensionTargetMatchesReceiver` treats an `int`
// extension as applying to `int?` receivers unconditionally (documented as
// relying on §26.2 forbidding this target, which nothing enforced).
// Post-fix: clean §26.2 rejection.
BEZEL_TEST(extension_target_kind_gate_4144,
           nullable_primitive_target_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "extension int? "
        "  public method Describe4144(): string => \"nullable-int\"; "
        "end; "
        "public static class Program4144b "
        "  public static method Main(): int "
        "    var x: int? := 5; "
        "    Console.WriteLine(x.Describe4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot extend a nullable type directly"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "26.2"));
}

// §26.2 "Function types". Pre-fix: compiled and RAN, with the extension
// member's body degraded to a no-op via the #3079 internal backstop (no
// crash, but a program the closed list says should never have compiled).
// Post-fix: clean §26.2 rejection.
BEZEL_TEST(extension_target_kind_gate_4144,
           function_type_target_rejected_not_silently_accepted) {
    auto r = runAot(
        "module M; "
        "extension (int) -> int "
        "  public method Describe4144(): string => \"fn\"; "
        "end; "
        "public static class Program4144c "
        "  public static method Main(): int "
        "    var f: (int) -> int := lambda(x) => x + 1; "
        "    Console.WriteLine(f.Describe4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot extend a function type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "26.2"));
}

// §26.2 "Inline-array types" (`extension [N]T`). Pre-fix: the DECLARATION
// compiled clean (dead, uncallable members) -- only a call site was ever
// rejected, by an unrelated blanket member-lookup gate. Post-fix: the
// declaration itself is rejected at the target-kind gate, before any call
// site is even reached.
BEZEL_TEST(extension_target_kind_gate_4144,
           inline_array_target_rejected_at_declaration) {
    auto r = runAot(
        "module M; "
        "extension [3]int "
        "  public method Describe4144(): string => \"inline-array\"; "
        "end; "
        "public static class Program4144d "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot extend an inline-array type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "26.2"));
}

// §26.2 through a type alias: an alias to a choice is still, transparently,
// a choice -- the closed list must not be bypassable by aliasing.
BEZEL_TEST(extension_target_kind_gate_4144,
           choice_target_via_alias_rejected) {
    auto r = runAot(
        "module M; "
        "public choice ShapeAliased4144 "
        "  Circle(radius: float), "
        "  Square(side: float); "
        "end ShapeAliased4144; "
        "type ShapeAliasT4144 := ShapeAliased4144; "
        "extension ShapeAliasT4144 "
        "  public method Describe4144(): string => (case this of "
        "    Circle(_) => \"circle\", "
        "    Square(_) => \"square\", "
        "  ); "
        "end; "
        "public static class Program4144e "
        "  public static method Main(): int "
        "    var s: ShapeAliased4144 := ShapeAliased4144.Circle(1.0); "
        "    Console.WriteLine(s.Describe4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot extend a choice type"));
}

// ---------------------------------------------------------------------------
// ACCEPT-SIDE CONTROLS (§15 of the lane rules): every §26.2-admitted target
// kind must keep compiling AND running correctly. A false-reject here would
// be worse than the crash this fix closes.
// ---------------------------------------------------------------------------

BEZEL_TEST(extension_target_kind_gate_4144, class_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public class Widget4144 "
        "  public property Name: string := \"w\"; "
        "end Widget4144; "
        "extension Widget4144 "
        "  public method Shout4144(): string => this.Name + \"!\"; "
        "end; "
        "public static class Program4144f "
        "  public static method Main(): int "
        "    var w := new Widget4144(); "
        "    Console.WriteLine(w.Shout4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "w!\n");
}

BEZEL_TEST(extension_target_kind_gate_4144, interface_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public interface IGreeter4144 "
        "  method Name4144(): string; "
        "end IGreeter4144; "
        "public class Person4144: IGreeter4144 "
        "  public method Name4144(): string => \"Ann\"; "
        "end Person4144; "
        "extension IGreeter4144 "
        "  public method Greet4144(): string => \"Hello, \" + this.Name4144(); "
        "end; "
        "public static class Program4144g "
        "  public static method Main(): int "
        "    var p: IGreeter4144 := new Person4144(); "
        "    Console.WriteLine(p.Greet4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Hello, Ann\n");
}

BEZEL_TEST(extension_target_kind_gate_4144, record_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public record Point4144(X: int, Y: int); "
        "extension Point4144 "
        "  public method Magnitude4144(): int => this.X * this.X + this.Y * this.Y; "
        "end; "
        "public static class Program4144h "
        "  public static method Main(): int "
        "    var p := new Point4144(3, 4); "
        "    Console.WriteLine(p.Magnitude4144().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "25\n");
}

BEZEL_TEST(extension_target_kind_gate_4144, enum_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public enum Color4144 "
        "  Red, Green, Blue; "
        "end Color4144; "
        "extension Color4144 "
        "  public method Label4144(): string => (case this of "
        "    Red => \"red\", "
        "    Green => \"green\", "
        "    Blue => \"blue\", "
        "  ); "
        "end; "
        "public static class Program4144i "
        "  public static method Main(): int "
        "    var c := Color4144.Green; "
        "    Console.WriteLine(c.Label4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "green\n");
}

BEZEL_TEST(extension_target_kind_gate_4144, primitive_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "extension int "
        "  public method Doubled4144(): int => this * 2; "
        "end; "
        "public static class Program4144j "
        "  public static method Main(): int "
        "    var x := 21; "
        "    Console.WriteLine(x.Doubled4144().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(extension_target_kind_gate_4144, object_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "public class Widget4144k "
        "  public property Name: string := \"w\"; "
        "end Widget4144k; "
        "extension Object "
        "  public method Tag4144(): string => \"obj\"; "
        "end; "
        "public static class Program4144k "
        "  public static method Main(): int "
        "    var x: Object := new Widget4144k(); "
        "    Console.WriteLine(x.Tag4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "obj\n");
}

// §26.5's own worked example: a generic extension over a class (`Array<T>`).
BEZEL_TEST(extension_target_kind_gate_4144, generic_class_target_still_accepted) {
    auto r = runAot(
        "module M; "
        "extension Array<T> "
        "  public method First4144(): T "
        "    if this.Count = 0 then throw new InvalidOperationException(\"empty array\"); end; "
        "    return this[0]; "
        "  end First4144; "
        "end; "
        "public static class Program4144l "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    Console.WriteLine(xs.First4144().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §26.4's own worked example: a generic extension over an interface with a
// `where` clause, applied to a nullable-carrying instantiation -- confirms
// the new top-level-nullable check does NOT reach into generic-argument
// positions (only the target's own head is checked).
BEZEL_TEST(extension_target_kind_gate_4144,
           generic_interface_where_clause_still_accepted) {
    auto r = runAot(
        "module M; "
        "extension IEnumerable<T> where T: value "
        "  public method FirstOrThrow4144(): T "
        "    for var item in this do return item; end; "
        "    throw new InvalidOperationException(\"empty sequence\"); "
        "  end FirstOrThrow4144; "
        "end; "
        "public static class Program4144m "
        "  public static method Main(): int "
        "    var xs: Array<int> := [7, 8, 9]; "
        "    Console.WriteLine(xs.FirstOrThrow4144().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// A bare `T` in a generic-argument position unifies with a NULLABLE receiver
// argument (`Array<int?>`) per §26.4 -- this is NOT an instance of §26.2's
// "nullable types directly" rule, which only forbids marking the target's
// own head nullable (`extension int?`).
BEZEL_TEST(extension_target_kind_gate_4144,
           generic_arg_nullable_instantiation_still_accepted) {
    auto r = runAot(
        "module M; "
        "extension Array<T> "
        "  public method Describe4144(): string => \"count=\" + this.Count.ToString(); "
        "end; "
        "public static class Program4144n "
        "  public static method Main(): int "
        "    var xs: Array<int?> := [1, null, 3]; "
        "    Console.WriteLine(xs.Describe4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "count=3\n");
}

// A type alias to an ALLOWED kind (class) must still be extendable through
// the alias name itself -- the alias-transparency check in the fix must not
// over-reject a legitimate alias target.
BEZEL_TEST(extension_target_kind_gate_4144, alias_to_class_still_accepted) {
    auto r = runAot(
        "module M; "
        "public class Widget4144o "
        "  public property Name: string := \"w\"; "
        "end Widget4144o; "
        "type WidgetAlias4144 := Widget4144o; "
        "extension WidgetAlias4144 "
        "  public method Shout4144(): string => this.Name + \"!\"; "
        "end; "
        "public static class Program4144o "
        "  public static method Main(): int "
        "    var w := new Widget4144o(); "
        "    Console.WriteLine(w.Shout4144()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "w!\n");
}

// §26.2 "Type parameters" (`extension T` is meaningless) is NOT given a
// dedicated reject by this fix (see file header) -- a bare, unresolvable
// identifier target still gets the pre-existing, general "unresolved type
// name" diagnostic, unaffected by the new gate.
BEZEL_TEST(extension_target_kind_gate_4144,
           bare_unresolved_identifier_target_unaffected_control) {
    auto r = runAot(
        "module M; "
        "extension NoSuchType4144 "
        "  public method Describe4144(): string => \"bare\"; "
        "end; "
        "public static class Program4144p "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "unresolved type name"));
}

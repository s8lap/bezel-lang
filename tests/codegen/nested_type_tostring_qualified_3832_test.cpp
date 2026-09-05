// Regression tests for #3832 (spec §8.5 / §11.6, v0.291; stdlib §2.13 rule 1,
// ruling on #3542): §8.5's default `ToString` for a NESTED class/enum, and
// stdlib §2.13's `Type.FullName` rendering it is pinned to, must render the
// full ENCLOSING-TYPE PATH, not just the declaring module.
//
// AUTHORITY, verbatim.
//
// spec §11.6 (v0.291), "Nested types":
//
//     "A nested type is referenced from outside as `Outer.Nested`, and the
//      enclosing path is part of its qualified name wherever one is
//      rendered: `Type.FullName` and [§8.5]'s default `ToString` render
//      `"M.Outer.Nested"` for a nested type declared in module `M`, while
//      `Type.Name` renders `"Nested"`."
//
// spec §8.5 (v0.291), "`ToString`", the classes bullet:
//
//     "For classes: the type's qualified name — the module-path-qualified
//      rendering stdlib §2.13 defines for `Type.FullName` (`"BankingApp.
//      BankAccount"`; a nested class carries its enclosing-type path —
//      `"BankingApp.Ledger.Entry"` for `Entry` declared inside `Ledger`,
//      [§11.6])."
//
// stdlib §2.13 (v0.291), "The `Type` class", rule 1:
//
//     "for a nested type (language spec §11.6) Name is the nested identifier
//      alone (`"Inner"`) and FullName is the declaring module, then every
//      enclosing type outermost first … then the identifier (`"M.Outer.
//      Inner"`), the enclosing-type path being part of the type's identity
//      (language spec §2.8.3 rule 3), so two unrelated nested types of one
//      name render distinct FullNames and the same Name."
//
// WHAT WAS WRONG (measured on f19de07, this issue's filing tree, and
// re-measured here on the same tree before this fix). `spec85QualifiedTypeName`
// (`LlvmBackend_impl.hpp`, the §8.5 class/enum default) and
// `spec85TypeRefQualifiedName` (the stdlib §2.13 rule-3 generic-arg rendering,
// used e.g. for a nested type appearing as a `Dictionary<K, V>` type argument)
// both composed `moduleName + "." + ts->name` — the BARE simple name — which
// silently dropped the enclosing-type path. A nested `Outer.Inner` in module
// `M` rendered `"M.Inner"`, indistinguishable from an unrelated top-level
// `Inner` declared directly in `M`. `TypeSymbol::qualifiedTypeName()`
// (Symbol.hpp, populated by fix #3529 for exactly this §11.6/§2.8.3-rule-3
// purpose) already builds the "Outer.Inner" half; the fix is switching both
// renderers onto it rather than the bare `ts->name` / `declaration->name`.
//
// Mechanical verification (see the commit message): reverting the two
// `qualifiedTypeName()` call sites back to the bare name in
// `spec85QualifiedTypeName` / `spec85TypeRefQualifiedName` leaves every
// "…nested…" test below failing (rendering "M.Inner" instead of
// "M.Outer.Inner", etc.), while the top-level and record-head controls pass
// either way.
//
// runAotOut moves the observable to stdout and strips the trailing line
// ending, so the expectations are the bare rendered string.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string out(const std::string& prog) { return runAotOut(prog); }

}  // namespace

// ---------------------------------------------------------------------------
// The defect, one level of nesting.
// ---------------------------------------------------------------------------

// per §8.5 / §11.6 (v0.291), the §11.6 worked example's own naming
// ("Outer.Nested"): a nested class's default `ToString` renders the full
// enclosing path. Was "M.Inner".
BEZEL_TEST(nested_type_tostring_qualified_3832,
           nested_class_default_call_carries_enclosing_path) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public V: int := 1; "
        "  end Inner; "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var i := new Outer.Inner(); "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Outer.Inner");
}

// §18.5 dispatches `ToString` implicitly, so the interpolation hole must
// agree with the explicit call — the same property #2934 pinned for the
// (then module-only) qualified rendering.
BEZEL_TEST(nested_type_tostring_qualified_3832,
           nested_class_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public V: int := 1; "
        "  end Inner; "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var i := new Outer.Inner(); "
        "  var a := $\"{i}\"; "
        "  var b := i.ToString(); "
        "  Console.WriteLine((if a = b then a else \"DISAGREE\")); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Outer.Inner");
}

// per §10 / §11.6: the nested-type rule is not class-specific — §8.5's enum
// bullet ("for … enums: the type's qualified name") carries the same
// enclosing path.
BEZEL_TEST(nested_type_tostring_qualified_3832,
           nested_enum_default_call_carries_enclosing_path) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public enum Color Red, Green, Blue; end Color; "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var c: Outer.Color := Outer.Color.Green; "
        "  Console.WriteLine(c.ToString()); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Outer.Color");
}

// ---------------------------------------------------------------------------
// The path must compose to FULL DEPTH, not just the immediate parent — the
// same distinguishing case #3529's mangling fix used, applied here to the
// SURFACE rendering rather than the linker symbol.
// ---------------------------------------------------------------------------

BEZEL_TEST(nested_type_tostring_qualified_3832,
           three_level_nesting_path_composes_to_full_depth) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public class Mid "
        "    public class Inner "
        "      public V: int := 1; "
        "    end Inner; "
        "  end Mid; "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var i := new Outer.Mid.Inner(); "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Outer.Mid.Inner");
}

// ---------------------------------------------------------------------------
// stdlib §2.13 rule 1's own stated consequence — "two unrelated nested types
// of one name render distinct FullNames". Sibling to #3529's mangling-
// collision rows, but pinning the SURFACE rendering rather than the linker
// symbol or comparator dispatch.
// ---------------------------------------------------------------------------

BEZEL_TEST(nested_type_tostring_qualified_3832,
           same_named_nested_types_render_distinct_full_names) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer1 "
        "  public class Ord "
        "    public V: int := 1; "
        "  end Ord; "
        "end Outer1; "
        "class Outer2 "
        "  public class Ord "
        "    public V: int := 1; "
        "  end Ord; "
        "end Outer2; "
        "class Program public static method Main(): int "
        "  var a := new Outer1.Ord(); "
        "  var b := new Outer2.Ord(); "
        "  Console.WriteLine($\"{a.ToString()} {b.ToString()}\"); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Outer1.Ord M.Outer2.Ord");
}

// ---------------------------------------------------------------------------
// stdlib §2.13 rule 3's generic-arg recursion (`spec85TypeRefQualifiedName`):
// a nested type used as a GENERIC TYPE ARGUMENT must carry its own enclosing
// path too, exactly as the bare form does.
// ---------------------------------------------------------------------------

BEZEL_TEST(nested_type_tostring_qualified_3832,
           nested_type_as_generic_argument_carries_enclosing_path) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public V: int := 1; "
        "  end Inner; "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var i := new Outer.Inner(); "
        "  var d := new Dictionary<string, Outer.Inner>(); "
        "  d.Add(\"k\", i); "
        "  Console.WriteLine($\"{d}\"); "
        "  return 0; "
        "end Main; end Program; "),
        "System.Collections.Generic.Dictionary<string, M.Outer.Inner>");
}

// ---------------------------------------------------------------------------
// Controls: top-level types are untouched — `enclosingTypePath` is empty for
// them, so `qualifiedTypeName()` degrades to the bare name and the mangled
// rendering is byte-identical to the pre-fix spelling.
// ---------------------------------------------------------------------------

// CONTROL — a lone top-level class never had an enclosing path; its default
// rendering must be exactly what #2934 already pinned ("M.Top", no change).
BEZEL_TEST(nested_type_tostring_qualified_3832,
           top_level_class_default_unchanged_control) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Top "
        "  public V: int := 1; "
        "end Top; "
        "class Program public static method Main(): int "
        "  var t := new Top(); "
        "  Console.WriteLine(t.ToString()); "
        "  return 0; "
        "end Main; end Program; "),
        "M.Top");
}

// CONTROL — a nested RECORD's positional head stays UNQUALIFIED per §8.5's
// record bullet regardless of nesting; this fix must not have qualified it.
BEZEL_TEST(nested_type_tostring_qualified_3832,
           nested_positional_record_head_stays_unqualified_control) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "class Outer "
        "  public record Pt(X: int, Y: int); "
        "end Outer; "
        "class Program public static method Main(): int "
        "  var p := new Outer.Pt(10, 20); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end Program; "),
        "Pt(X: 10, Y: 20)");
}

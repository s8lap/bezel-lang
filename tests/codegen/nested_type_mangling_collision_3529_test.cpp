// Regression tests for fix #3529 — `Emitter::mangledTypeName` built the CLASS
// component of every emitted symbol from the type's SIMPLE name, so two nested
// types with the same simple name in one module collapsed onto one symbol.
//
// AUTHORITY. spec §2.8.3 rule 3 (v0.283), "Program-wide type identity",
// verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, [§11.6]), and
//      its generic arity**; a constructed generic type is identified by its
//      generic's identity plus the identities of its arguments."
//
// and spec §11.6 (v0.283), "Nested types", verbatim on the path itself:
//
//     "A nested type is referenced from outside as `Outer.Nested`."
//     "Nested types may themselves contain nested types."
//
// #1488 gave `mangledTypeName` the DECLARING MODULE half of that identity
// ("M.Ord"). The ENCLOSING-TYPE half was never supplied, so `Outer1.Ord` and
// `Outer2.Ord` — "two unrelated types" under rule 3 — both mangled to "M.Ord".
//
// THE DEFECT, MEASURED on b43196a (and byte-identically on ef339fa, i.e. before
// #3488/#3512 — this half is older than the module half's fix):
//
//   * ONE `bz$classcmp$M.Ord` thunk served both types (`nm`), so the second
//     type sorted by the FIRST type's `CompareTo`: the issue's own repro
//     printed "asc=12 desc=12" where "asc=12 desc=21" is required.
//   * Every other symbol built from the TypeSymbol was LLVM auto-uniquified
//     instead: `bz$M.Ord$CompareTo` + `bz$M.Ord$CompareTo.1`, `$ctor` + `$ctor.2`,
//     `$Dispose` + `.3`, `$DisposeCore` + `.4`, `$init` + `.5`, `td$M.Ord` + `.6`,
//     `vt$M.Ord` + `.13` — which is verbatim the failure #1488's own comment in
//     LlvmBackend_impl.hpp describes ("a symbol that exists nowhere").
//   * With DIFFERENT FIELD LAYOUTS the shared comparator is not merely wrong, it
//     is unsound: the layout row below SEGV'd (rc=139) pre-fix, reading one
//     type's leading `int` as the other's leading `bz.string*`.
//
// THE FIX carries the missing half on the symbol — `TypeSymbol::enclosingTypePath`,
// populated at symbol-creation time by SymbolCollection for a local declaration
// and by ModuleLoader for an imported one, exactly as #1488's `moduleName` is —
// and joins it in ONE place, `Emitter::mangledTypeName`. A top-level type's path
// is empty, so its mangled name is byte-identical to the pre-fix spelling; the
// controls at the bottom of this file are what pin that.
//
// The mangled nesting separator is `+`, not `.` — `bz$M.Outer1+Ord$ctor`. Rule
// 3's identity is a TUPLE and a module name is itself dotted, so a dotted join
// cannot tell `module M.Outer1;` + `class Ord` from `module M;` + `class Outer1`
// + nested `class Ord`; that shape links today and would stop linking. The
// measured evidence and the guarding row live in
// tests/module/xmod_nested_type_mangling_3529_test.cpp, since it takes two
// artifacts to observe.
//
// THE OBSERVABLE IS THE ANSWER, NOT THE SYMBOL NAME. An ordering defect is a
// permutation of the same elements, so every row compares the program's stdout
// (or, for the unsound row, its stdout AND the fact that it ran at all — a
// signalled process loses buffered stdout and must not read as a wrong answer).
// Asserting on `nm` output would pin the mangling rather than the behaviour it
// exists to protect.
//
// The cross-module half — that the exporter's definition and the importer's
// external declaration agree on the new spelling — lives in
// tests/module/xmod_nested_type_mangling_3529_test.cpp, because it needs a
// two-artifact build the single-source AOT harness cannot express.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The issue's own repro, verbatim in shape: two same-named nested classes in one
// module with OPPOSITE `CompareTo`.
const char* kTwoNestedOrds =
    "module M; "
    "class Outer1 "
    "  public class Ord: IComparable<Outer1.Ord> "
    "    private _v: int; "
    "    public constructor(v: int) _v := v; end constructor; "
    "    public method Value(): int => _v; "
    "    public method CompareTo(other: Outer1.Ord): int => _v - other.Value(); "
    "  end Ord; "
    "end Outer1; "
    "class Outer2 "
    "  public class Ord: IComparable<Outer2.Ord> "
    "    private _v: int; "
    "    public constructor(v: int) _v := v; end constructor; "
    "    public method Value(): int => _v; "
    "    public method CompareTo(other: Outer2.Ord): int "
    "      => other.Value() - _v; "
    "  end Ord; "
    "end Outer2; ";

std::string prog(const std::string& decls, const std::string& body) {
    return decls + "class Program public static method Main(): int " + body +
           " end Main; end Program; ";
}

}  // namespace

// ── The defect ─────────────────────────────────────────────────────────────

// per spec §2.8.3 rule 3 + §11.6 (v0.283) and stdlib §4.4's in-place
// `Sort()`, #3529: `Outer1.Ord` and `Outer2.Ord`
// are two unrelated types, so each must sort by its own `CompareTo`. Pre-fix
// both mangled to "M.Ord", one `bz$classcmp$M.Ord` served both, and the
// descending half printed the ASCENDING answer ("asc=123 desc=123").
BEZEL_TEST(nested_type_mangling_collision_3529,
           two_same_named_nested_types_use_their_own_comparator) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(kTwoNestedOrds,
            "  using var p1 := new Outer1.Ord(2); "
            "  using var p2 := new Outer1.Ord(3); "
            "  using var p3 := new Outer1.Ord(1); "
            "  var pa: Array<Outer1.Ord> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Outer2.Ord(2); "
            "  using var q2 := new Outer2.Ord(3); "
            "  using var q3 := new Outer2.Ord(1); "
            "  var qa: Array<Outer2.Ord> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}"
            "{pa[2].Value()} desc={qa[0].Value()}{qa[1].Value()}"
            "{qa[2].Value()}\"); "
            "  return 0; ")),
        std::string("asc=123 desc=321"));
}

// per spec §11.6 (v0.283) "Nested types may themselves contain nested types",
// #3529: the path must compose to FULL DEPTH, not just one level. `Outer.Mid.Ord`
// and `Other.Mid.Ord` differ only in their OUTERMOST enclosing type, so a fix
// that qualified by the immediate parent alone would leave both at "M.Mid+Ord"
// and this row would still print "desc=123". It is the row that distinguishes
// "threads the whole chain" from "prepends the nearest name".
BEZEL_TEST(nested_type_mangling_collision_3529,
           three_level_nesting_path_composes_to_full_depth) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "module M; "
            "class Outer "
            "  public class Mid "
            "    public class Ord: IComparable<Outer.Mid.Ord> "
            "      private _v: int; "
            "      public constructor(v: int) _v := v; end constructor; "
            "      public method Value(): int => _v; "
            "      public method CompareTo(other: Outer.Mid.Ord): int "
            "        => _v - other.Value(); "
            "    end Ord; "
            "  end Mid; "
            "end Outer; "
            "class Other "
            "  public class Mid "
            "    public class Ord: IComparable<Other.Mid.Ord> "
            "      private _v: int; "
            "      public constructor(v: int) _v := v; end constructor; "
            "      public method Value(): int => _v; "
            "      public method CompareTo(other: Other.Mid.Ord): int "
            "        => other.Value() - _v; "
            "    end Ord; "
            "  end Mid; "
            "end Other; ",
            "  using var p1 := new Outer.Mid.Ord(2); "
            "  using var p2 := new Outer.Mid.Ord(3); "
            "  using var p3 := new Outer.Mid.Ord(1); "
            "  var pa: Array<Outer.Mid.Ord> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Other.Mid.Ord(2); "
            "  using var q2 := new Other.Mid.Ord(3); "
            "  using var q3 := new Other.Mid.Ord(1); "
            "  var qa: Array<Other.Mid.Ord> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}"
            "{pa[2].Value()} desc={qa[0].Value()}{qa[1].Value()}"
            "{qa[2].Value()}\"); "
            "  return 0; ")),
        std::string("asc=123 desc=321"));
}

// per spec §2.8.3 rule 3 + §11.6, v0.283, #3529: with DIFFERENT FIELD LAYOUTS
// the shared thunk is unsound, not merely wrong — `Outer1.Ord` stores an `int`
// where `Outer2.Ord` stores a `string`, and the single pre-fix thunk
// dereferences one as the other. `Outer2.Ord` is sorted FIRST so it owns the
// shared thunk and `Outer1.Ord`'s `int` is the value read as a `bz.string*`.
// MEASURED pre-fix on b43196a: the program printed "q=cbbaaa" and then died with
// rc=139 (SIGSEGV) before the second line.
//
// `ran` and the output are asserted separately on purpose: a signalled process
// loses its buffered stdout, so an output-only assertion could not tell a crash
// from a wrong answer — which is the distinction this row exists to make.
BEZEL_TEST(nested_type_mangling_collision_3529,
           differing_field_layouts_do_not_reinterpret_one_type_as_the_other) {
    auto r = runAot(prog(
        "module M; "
        "class Outer1 "
        "  public class Ord: IComparable<Outer1.Ord> "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method CompareTo(other: Outer1.Ord): int "
        "      => _v - other.Value(); "
        "  end Ord; "
        "end Outer1; "
        "class Outer2 "
        "  public class Ord: IComparable<Outer2.Ord> "
        "    private _name: string; "
        "    public constructor(n: string) _name := n; end constructor; "
        "    public method Name(): string => _name; "
        "    public method CompareTo(other: Outer2.Ord): int "
        "      => _name.Length - other.Name().Length; "
        "  end Ord; "
        "end Outer2; ",
        "  using var q1 := new Outer2.Ord(\"bb\"); "
        "  using var q2 := new Outer2.Ord(\"c\"); "
        "  using var q3 := new Outer2.Ord(\"aaa\"); "
        "  var qa: Array<Outer2.Ord> := [q1, q2, q3]; "
        "  qa.Sort(); "
        "  Console.WriteLine($\"q={qa[0].Name()}{qa[1].Name()}{qa[2].Name()}\"); "
        "  using var p1 := new Outer1.Ord(2); "
        "  using var p2 := new Outer1.Ord(3); "
        "  using var p3 := new Outer1.Ord(1); "
        "  var pa: Array<Outer1.Ord> := [p1, p2, p3]; "
        "  pa.Sort(); "
        "  Console.WriteLine($\"p={pa[0].Value()}{pa[1].Value()}{pa[2].Value()}\"); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("q=cbbaaa\np=123\n"));
}

// ── Controls: these pass on BOTH trees ─────────────────────────────────────

// CONTROL (spec §11.6, v0.283) — a lone nested comparable type never collided
// with anything, so it must be untouched. If this row ever fails, the rows above
// are measuring the harness rather than the compiler.
BEZEL_TEST(nested_type_mangling_collision_3529,
           single_nested_comparable_type_unchanged_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "module M; "
            "class Outer "
            "  public class Ord: IComparable<Outer.Ord> "
            "    private _v: int; "
            "    public constructor(v: int) _v := v; end constructor; "
            "    public method Value(): int => _v; "
            "    public method CompareTo(other: Outer.Ord): int "
            "      => _v - other.Value(); "
            "  end Ord; "
            "end Outer; ",
            "  using var p1 := new Outer.Ord(2); "
            "  using var p2 := new Outer.Ord(3); "
            "  using var p3 := new Outer.Ord(1); "
            "  var pa: Array<Outer.Ord> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}"
            "{pa[2].Value()}\"); "
            "  return 0; ")),
        std::string("asc=123"));
}

// CONTROL (spec §2.8.3 rule 3, v0.283) — two DIFFERENTLY-named nested types in
// ONE enclosing class never shared a mangled name ("M.Asc" / "M.Desc"), so they
// were already correct. This separates "the fix works" from "nested comparison
// was broken in general".
BEZEL_TEST(nested_type_mangling_collision_3529,
           differently_named_nested_types_never_collided_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "module M; "
            "class Outer "
            "  public class Asc: IComparable<Outer.Asc> "
            "    private _v: int; "
            "    public constructor(v: int) _v := v; end constructor; "
            "    public method Value(): int => _v; "
            "    public method CompareTo(other: Outer.Asc): int "
            "      => _v - other.Value(); "
            "  end Asc; "
            "  public class Desc: IComparable<Outer.Desc> "
            "    private _v: int; "
            "    public constructor(v: int) _v := v; end constructor; "
            "    public method Value(): int => _v; "
            "    public method CompareTo(other: Outer.Desc): int "
            "      => other.Value() - _v; "
            "  end Desc; "
            "end Outer; ",
            "  using var p1 := new Outer.Asc(2); "
            "  using var p2 := new Outer.Asc(3); "
            "  using var p3 := new Outer.Asc(1); "
            "  var pa: Array<Outer.Asc> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Outer.Desc(2); "
            "  using var q2 := new Outer.Desc(3); "
            "  using var q3 := new Outer.Desc(1); "
            "  var qa: Array<Outer.Desc> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}"
            "{pa[2].Value()} desc={qa[0].Value()}{qa[1].Value()}"
            "{qa[2].Value()}\"); "
            "  return 0; ")),
        std::string("asc=123 desc=321"));
}

// CONTROL (spec §2.8.3 rule 3, v0.283) — two TOP-LEVEL classes in one module.
// Their enclosing-type path is empty, so their mangled names ("M.Asc"/"M.Desc")
// are byte-identical before and after the fix. This is the row that pins the
// non-nested world as unchanged: the whole emitted-symbol namespace runs through
// `mangledTypeName`, and a regression there would be a very wide one.
BEZEL_TEST(nested_type_mangling_collision_3529,
           top_level_types_mangling_unchanged_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "module M; "
            "class Asc: IComparable<Asc> "
            "  private _v: int; "
            "  public constructor(v: int) _v := v; end constructor; "
            "  public method Value(): int => _v; "
            "  public method CompareTo(other: Asc): int => _v - other.Value(); "
            "end Asc; "
            "class Desc: IComparable<Desc> "
            "  private _v: int; "
            "  public constructor(v: int) _v := v; end constructor; "
            "  public method Value(): int => _v; "
            "  public method CompareTo(other: Desc): int "
            "    => other.Value() - _v; "
            "end Desc; ",
            "  using var p1 := new Asc(2); "
            "  using var p2 := new Asc(3); "
            "  using var p3 := new Asc(1); "
            "  var pa: Array<Asc> := [p1, p2, p3]; "
            "  pa.Sort(); "
            "  using var q1 := new Desc(2); "
            "  using var q2 := new Desc(3); "
            "  using var q3 := new Desc(1); "
            "  var qa: Array<Desc> := [q1, q2, q3]; "
            "  qa.Sort(); "
            "  Console.WriteLine($\"asc={pa[0].Value()}{pa[1].Value()}"
            "{pa[2].Value()} desc={qa[0].Value()}{qa[1].Value()}"
            "{qa[2].Value()}\"); "
            "  return 0; ")),
        std::string("asc=123 desc=321"));
}


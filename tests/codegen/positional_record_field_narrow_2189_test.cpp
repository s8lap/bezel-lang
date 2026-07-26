// Codegen (AOT) — §7.4 readonly-field narrowing for positional-record fields
// (v0.226, #2189). §9.1: a record's positional fields "become public read-only
// members"; §9.4: "Properties auto-generated from positional fields are
// read-only"; §7.4 (v0.214): "a nullable readonly field — instance or static —
// participates in flow narrowing". But the positional FieldSymbol synthesized
// in SymbolCollection's `td.hasPositionalFields` loop carried no "readonly"
// modifier, so ExprTC::isReadonlyField (which scans for that token) returned
// false for every positional field, and readonlyFieldPathOf declined the path —
// `r.Name <> null` over `record R(Name: string?)` established nothing, the
// guard-then-use was rejected, and ExprTC::fieldTypeOf's posParam branch (which
// follows an isReadonlyField pass) was dead. The fix recognizes a posParam
// field whose parent is a Record as readonly in isReadonlyField (§9.1: a
// positional-record field is read-only by construction; §9.4).
//
// Safety trace (per the issue): §9.5 `with` produces a NEW record value, so
// `r := r with(...)` rebinds the root `r` and the `local._f` root-drop regime
// drops the fact; records are value types, so by-value passing copies and
// `ref`/`out` invalidates per §7.4; §9.7 borrow-bearing records narrow on
// field-nullness, orthogonal to the borrow's lifetime. A CLASS primary-
// constructor parameter (§22.10) is captured into a private field that is NOT
// public-readonly (an `owned` capture is disposed with the object — mutated),
// so the fix's Record-kind gate refuses it.
//
// ACCEPT cells: FAILS-WITHOUT (member access on a nullable positional field is
// a "requires a local copy first" compile error → runAot does not run, -999);
// PASSES-WITH (narrows per §7.4, compiles, returns the stated value). The
// REJECT guards pin that the fix does not over-narrow past positional fields.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// True iff sema rejected `src` with a diagnostic containing `needle`.
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §7.4 / §9.1 / §9.4 (#2189): a nullable positional-record field narrows after
// a null test — `r.Name <> null` admits `r.Name.Length` in the true branch
// without a copy-to-local. "hello".Length == 5.
BEZEL_TEST(positional_record_field_narrow_2189, null_test_narrows_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(Name: string?); "
        "class Prog public static method Main(): int "
        "  var r := new R(\"hello\"); "
        "  if r.Name <> null then return r.Name.Length; end; return 0; "
        "end Main; end Prog;"), 5);
}

// §7.4 / §9.1 (#2189): the null referent takes the else branch — the narrowed
// read never runs (no SEGV). Returns the else-branch sentinel.
BEZEL_TEST(positional_record_field_narrow_2189, null_value_takes_else) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(Name: string?); "
        "class Prog public static method Main(): int "
        "  var r := new R(null); "
        "  if r.Name <> null then return r.Name.Length; "
        "  else return 42; end; return 0; "
        "end Main; end Prog;"), 42);
}

// §7.4 / §9.1 / §9.7 (#2189): narrowing admits method dispatch through a
// class-typed positional field of a BORROW-BEARING record (§9.7 — narrowing
// tracks field-nullness, orthogonal to the carried borrow's lifetime).
// Logger.Tag() + 1 == 22.
BEZEL_TEST(positional_record_field_narrow_2189, class_typed_field_dispatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Logger "
        "  public method Tag(): int return 21; end Tag; "
        "  public constructor() end constructor; "
        "  destructor end destructor; end Logger; "
        "record R(Log: Logger?); "
        "class Prog public static method Main(): int "
        "  using var l := new Logger(); var r := new R(l); "
        "  if r.Log <> null then return r.Log.Tag() + 1; end; return 0; "
        "end Main; end Prog;"), 22);
}

// §7.4 / §9.4 (#2189) over-narrow guard: a MUTABLE body field is unchanged —
// the §7.4 TOCTOU refusal still stands (the fix keys on the posParam flag of a
// RECORD field, not on body fields). The narrowed read stays a compile error.
BEZEL_TEST(positional_record_field_narrow_2189, mutable_field_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _name <> null then return _name.Length; end; return 0; "
        "  end Len; end C;",
        "copy to a local first"));
}

// §7.4 / §22.10 (#2189) over-narrow guard: a CLASS primary-constructor
// parameter is captured into a private field (§22.10) that is NOT public-
// readonly (an `owned` capture is disposed with the object — mutated), so it
// must not narrow regardless of the posParam flag. The fix's Record-kind gate
// is what prevents this; the narrowed read stays a compile error.
BEZEL_TEST(positional_record_field_narrow_2189, class_primary_ctor_param_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; public class C(name: string?) "
        "  public method Len(): int "
        "    if name <> null then return name.Length; end; return 0; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"hey\"); return c.Len(); "
        "  end Main; end C;",
        "copy to a local first"));
}

// Regression tests for issue #5285 (follow-up to #5266 / #5210) — spec §14.1
// "Instantiation and its site" and §14.2 "Constraints" (v0.313), and §3.6
// rule 2.
//
// §14.1 lists, among the sites where a generic type is instantiated by being
// NAMED, "in a `where` bound" and "in a type alias (§27.8)", and says a rule
// that fires at the instantiation site — §14.2's constraint checks included —
// "fires at every such site".  #5266 recorded the annotation positions; these
// two are the remaining unrecorded ones, so `G<int>` in either position was
// wrongly ACCEPTED while the same `G<int>` in a field or local annotation
// correctly rejects.
//
// Oracle: reject rows count the diagnostics carrying the needle (exact count,
// so a missing OR a doubled report fails); accept rows must compile and run
// with exit code 0.

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Number of errors containing `needle`, or -1 if the program ran.
int errorsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (r.ran) return -1;
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            ++n;
    return n;
}

const char* kOperand = "`as` OPERAND against the reference target `IS`";
const char* kDefault = "does not satisfy the constraint `where K: value`";
const char* kAsTarget = "is used as an `as` target in the generic body";
const char* kCase = "is used as a `case` scrutinee against a reference-typed";

// G<T> does `v as IS` — a reference target over the bare operand. At `G<int>`
// the substitution is a value type, so §12.7 rejects it at the site.
const std::string kOperandG =
    "module M; interface IS end IS; "
    "class G<T> public method F(v: T): bool "
    "  var o := v as IS; return o <> null; end F; end G; ";
// §17.11 / §3.6 rule 2: `default(K)` under `where K: value` is rejected when
// K is bound to a zero-less enum.
const std::string kDefaultBox =
    "module M; enum Level Low := 1, Mid := 2, High := 3; end Level; "
    "enum Z Zero := 0, One := 1; end Z; "
    "class Box<K> where K: value public method Get(): K "
    "  var t := default(K); return t; end Get; end Box; ";
const std::string kMain =
    "class C public static method Main(): int return 0; end Main; end C;";

int operandErrors(const std::string& body) {
    return errorsWith(kOperandG + body + kMain, kOperand);
}

int defaultErrors(const std::string& body) {
    return errorsWith(kDefaultBox + body + kMain, kDefault);
}

}  // namespace

// ---- Repro A: a `where` bound on a class is an instantiation site ----

BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H<U> where U: G<int> end H;"), 1);
}

BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_accepted_control) {
    BEZEL_EXPECT_EQ(operandErrors(
        "interface J end J; class Impl : J public constructor() end; end Impl; "
        "class H<U> where U: G<Impl> end H;"), -1);
}

// A bound that is itself a type ARGUMENT of another constructed type recurses
// the same way an annotation does (#5266's `Array<G<int>>` row).
BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_nested_type_argument_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H<U> where U: Array<G<int>> end H;"), 1);
}

// A method's `where` bound is a site on the same footing: §14.1 says "a
// `where` bound" without restricting the carrier to a type declaration.
BEZEL_TEST(class_generic_where_bound_sites_5285, method_where_bound_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H public method M<U>(u: U) where U: G<int> end M; end H;"), 1);
}

// A `where` bound on an EXTENSION block is a site on the same footing: A.3
// places GenericConstraints on extension blocks too, so §14.1's "a `where`
// bound" reaches them without qualification.
BEZEL_TEST(class_generic_where_bound_sites_5285, extension_declaration_where_bound_rejected) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class Bag<T> public v: int; public constructor() v := 0; end; end Bag; "
        "extension Bag<T> where T: G<int> "
        "  public method Extra(): int return 1; end Extra; end;"), 1);
}

// The other §14.2 joins fire at a `where` bound too, not only the as-OPERAND
// one — the site recording is shared, so each join is pinned here.
BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_default_join_rejected_once) {
    BEZEL_EXPECT_EQ(defaultErrors(
        "class H<U> where U: Box<Level> end H;"), 1);
}

BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_case_join_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; interface IS end IS; "
        "class G<T> public method F(v: T): int "
        "  return (case v of var x: IS => 1, _ => 9); end F; end G; "
        "class H<U> where U: G<int> end H; " + kMain, kCase), 1);
}

// The as-TARGET join reaches a `where` bound through the Resolver's own
// binding rather than through recordAnnotationInstantiationSites (#5266
// notes the same for field/param sites), so this row passes with AND without
// the fix.  Kept as a pin that the fix neither removes nor doubles the
// diagnostic; the OPERAND/default/case rows above are the FAIL-WITHOUT ones.
BEZEL_TEST(class_generic_where_bound_sites_5285, class_where_bound_as_target_join_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; class Box<T> public method Probe(o: Object): int "
        "  var r := o as T; if r <> null then return 1; end; return 0; "
        "  end Probe; end Box; "
        "class H<U> where U: Box<[3]int> end H; " + kMain, kAsTarget), 1);
}

// ---- Repro B: a type alias is an instantiation site ----

BEZEL_TEST(class_generic_where_bound_sites_5285, type_alias_declaration_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "type A := G<int>;"), 1);
}

BEZEL_TEST(class_generic_where_bound_sites_5285, type_alias_declaration_accepted_control) {
    BEZEL_EXPECT_EQ(operandErrors(
        "interface J end J; class Impl : J public constructor() end; end Impl; "
        "type A := G<Impl>;"), -1);
}

// A generic alias's own body is a site too: `A<int>` forms `G<int>` through
// the alias, and §14.1 fires the rule at EVERY site — the alias declaration
// and the field naming the alias are two.  The in-process runAot sink folds
// same-text diagnostics, so a gtest count of 2 is not a faithful oracle; the
// per-site COUNT is pinned in
// tests/lit/sema/class_generic_alias_two_sites_5285.bzl (which counts 2 with
// the fix and 1 without it), and the class where bound + alias pair likewise
// in class_generic_where_bound_and_alias_sites_5285.bzl.
BEZEL_TEST(class_generic_where_bound_sites_5285, generic_type_alias_body_rejected_with_use) {
    BEZEL_EXPECT_EQ(operandErrors(
        "type A<T> := G<T>; class H private _a: A<int>?; end H;"), 1);
}

// An UNUSED generic alias never instantiates `G`, so nothing fires — the
// accept side of the row above.
BEZEL_TEST(class_generic_where_bound_sites_5285, unused_generic_type_alias_accepted) {
    BEZEL_EXPECT_EQ(operandErrors(
        "type A<T> := G<T>;"), -1);
}

// The alias's own `where` bound is a site on the same footing as its body.
BEZEL_TEST(class_generic_where_bound_sites_5285, type_alias_where_bound_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "type A<T> := T where T: G<int>;"), 1);
}

BEZEL_TEST(class_generic_where_bound_sites_5285, type_alias_default_join_rejected_once) {
    BEZEL_EXPECT_EQ(defaultErrors(
        "type A := Box<Level>;"), 1);
}

// A used alias still rejects, and the DECLARATION is a site in its own right
// per §14.1 ("in a type alias (§27.8)").  This row's count is 1 both with and
// without the fix — the field naming the alias already fired through #5266's
// path — so it is NOT the pin for the declaration site; the declaration-only
// row above is.  Kept as the control that adding a use does not change the
// verdict.
BEZEL_TEST(class_generic_where_bound_sites_5285, used_type_alias_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "type A := G<int>; class H private _a: A?; end H;"), 1);
}

// ---- Accept control: every site with a reference substitution compiles ----

BEZEL_TEST(class_generic_where_bound_sites_5285, reference_substitution_accepted_everywhere) {
    auto r = runAot(
        "module M; interface IS end IS; "
        "interface J end J; class Impl : IS, J public constructor() end; end Impl; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "type A := G<Impl>; "
        "type B<T> := G<T> where T: G<Impl>; "
        "class H<U> where U: G<Impl> "
        "  public method M<V>(v: V): int where V: G<Impl> return 0; end M; end H; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

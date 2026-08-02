// fix #2466 (Appendix A.3 InheritItem grammar note, §9.6, §11.2, v0.236):
// `Parser::parseInheritance` and the primary-ctor-class branch of
// `Parser::parseTypeOrInterfaceDecl` silently tokenized away a
// parenthesized argument list on every interface-position `InheritItem`
// for record and interface decls (and on items 1+ for a primary-ctor
// class) — `record R: IFoo(1, 2)` and `interface I: IFoo(1)` compiled
// with NO diagnostic, the `(args)` vanishing with no AST node. The bug is
// distinct from #2455 (a data-loss bug on class-header base-ctor args);
// here there is no data to lose because interfaces have no constructors,
// but Appendix A.3's own note on `InheritItem` documents the restriction
// ("the argument list is permitted only on the base class (the first
// item), not on interfaces"), so the silent tokenize-away is upgraded to
// a diagnostic. Only an EMPTY arg list (`IFoo()`) stays accepted — it
// drops no data, matching #2455's carve-out and the pre-existing
// parser_generics_inheritance.inheritance_with_unparsed_arg_list_
// does_not_error regression test.
//
// THE SPEC READING (established before coding, not silently chosen):
//   - Appendix A.3: `InheritItem ::= Type ("(" ArgList? ")")?` with the
//     note "the argument list is permitted only on the base class (the
//     first item), not on interfaces" (v0.236).
//   - §9.6: "A record cannot derive from another record or from a class
//     (other than the implicit `Object` root). A record may implement any
//     number of interfaces" — so every InheritItem on a record is, by
//     construction, an interface position.
//   - §11.2: "An interface may extend other interfaces" (and Bezel
//     interfaces have no constructors) — so every InheritItem on an
//     interface is likewise an interface position.
// An arg list written in any of those positions was therefore always
// inert nonsense; the question was only whether to keep accepting it
// silently. Per CLAUDE.md rule 4, the simplest reading consistent with
// Appendix A.3's stated restriction is REJECTION of the non-empty form.
//
// This file tests the parser-level diagnostic in isolation (fast, no
// codegen).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

bool diagContains(const bezel::test::ParseResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// --- Rejections: non-empty arg list on an interface position ----------

// §9.6, v0.236: a record's every inheritance item is an interface, so an
// arg list on the first (or any) item is rejected per Appendix A.3's
// `InheritItem` note. The issue's own illustrative repro, verbatim.
BEZEL_TEST(parser_inherit_arglist_2466,
           record_first_item_arglist_rejected_appendix_a_9_6) {
    auto r = parseAll(
        "module M;"
        "public interface IMarker end IMarker;"
        "public record Point(X: int, Y: int): IMarker(1, 2)"
        "end Point;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "an argument list is not permitted here"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not on an interface"));
}

// §11.2, v0.236: an interface's every inheritance item is an interface
// position too — same rejection, same wording.
BEZEL_TEST(parser_inherit_arglist_2466,
           interface_first_item_arglist_rejected_appendix_a_11_2) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo end IFoo;"
        "public interface IBar: IFoo(1)"
        "end IBar;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "an argument list is not permitted here"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not on an interface"));
}

// Multiple interface items: each one carrying an arg list is rejected
// (axis: count). Both items are interface positions for a record.
BEZEL_TEST(parser_inherit_arglist_2466,
           record_multiple_items_arglist_rejected_appendix_a) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo end IFoo;"
        "public interface IBar end IBar;"
        "public record R(X: int): IFoo(1), IBar(2)"
        "end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "an argument list is not permitted here"));
}

// Same shape on an interface extending two interfaces.
BEZEL_TEST(parser_inherit_arglist_2466,
           interface_multiple_items_arglist_rejected_appendix_a) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo end IFoo;"
        "public interface IBar end IBar;"
        "public interface IZed: IFoo(1), IBar(2)"
        "end IZed;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "an argument list is not permitted here"));
}

// Appendix A.3 InheritItem note + §11.1, v0.236: a primary-ctor class
// captures the FIRST item's arg list (legitimate base-ctor chain), but a
// non-empty arg list on items 1+ (interface positions) was silently
// discarded by the primary-ctor-class branch of parseTypeOrInterfaceDecl
// — same #2466 gap, distinct code site. Reject it with the same wording.
BEZEL_TEST(parser_inherit_arglist_2466,
           primary_ctor_interface_position_arglist_rejected_appendix_a) {
    auto r = parseAll(
        "module M;"
        "public interface IMarker end IMarker;"
        "public class Base(v: int) end Base;"
        "public class Derived(x: int): Base(x), IMarker(7)"
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "an argument list is not permitted here"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not on an interface"));
}

// --- Regression guards: legitimate shapes must NOT be rejected --------

// Bare inheritance (no arg list at all) — the overwhelmingly common case
// — must remain accepted for records, interfaces, and classes alike.
BEZEL_TEST(parser_inherit_arglist_2466,
           bare_inheritance_no_arglist_still_accepted) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo end IFoo;"
        "public interface IBar: IFoo end IBar;"
        "public record R(X: int): IFoo end R;"
        "public class Base end Base;"
        "public class D: Base, IFoo end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A generic argument list `<T>` is part of `Type` per Appendix A.3's
// `InheritItem ::= Type (...)`, NOT the optional `("ArgList?")` — so
// `IFoo<T>` (or `IFoo<int>`) must remain accepted. This is the axis the
// issue brief's symptom description conflated with the ctor-arg list; it
// was never broken and must stay unbroken.
BEZEL_TEST(parser_inherit_arglist_2466,
           generic_arg_list_in_type_still_accepted_appendix_a) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo<T> end IFoo;"
        "public interface IBar<T>: IFoo<T> end IBar;"
        "public record R(X: int): IFoo<int> end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An EMPTY arg list (`IFoo()`) drops no data — the #2455 / #2466
// carve-out. Stays accepted on every interface position (records,
// interfaces, primary-ctor class items 1+).
BEZEL_TEST(parser_inherit_arglist_2466,
           empty_arglist_still_accepted_appendix_a_carve_out) {
    auto r = parseAll(
        "module M;"
        "public interface IFoo end IFoo;"
        "public record R(X: int): IFoo() end R;"
        "public interface IBar: IFoo() end IBar;"
        "public class Base(v: int) end Base;"
        "public class D(x: int): Base(x), IFoo() end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A real (non-empty) primary-constructor base-ctor arg list on the FIRST
// item of a primary-ctor class is the documented §11.1 shorthand — must
// remain accepted (the new rejection fires only on items 1+).
BEZEL_TEST(parser_inherit_arglist_2466,
           primary_ctor_first_item_arglist_still_accepted_spec_11_1) {
    auto r = parseAll(
        "module M;"
        "public class Base(v: int) end Base;"
        "public class Derived(x: int): Base(x) end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

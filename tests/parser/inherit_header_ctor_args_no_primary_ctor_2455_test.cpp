// fix #2455 (§11.1 / §22.10 / §12.8, Appendix A `InheritItem`, v0.235):
// `Parser::parseInheritance` used to tokenize away a class-header base-
// constructor argument list (`class D: Base(99)`) whenever `D` had no
// primary-constructor parameter list of its own — the args vanished with NO
// AST node and NO diagnostic, so the base ran with defaults instead
// (confirmed for LOCAL bases, not merely cross-module).
//
// THE SPEC READING (established before coding, not silently chosen):
//   - §11.1's "the base's arguments appear in parentheses after the base
//     class name in the inheritance list" sentence lives inside the
//     "Primary constructors" subsection and is stated of a class that
//     already has its own primary-ctor parameter list.
//   - §22.10 states the header sugar "replaces an explicit base(...) call"
//     that a primary constructor's chain would otherwise need — again
//     exclusively a primary-constructor concept.
//   - §12.8 gives the GENERAL, unconditional rule for a class with neither a
//     primary constructor nor an explicit chain: "If neither base(...) nor
//     this(...) is written, the compiler inserts an implicit base() call" —
//     a no-argument call, with no textual provision for a header argument
//     list to feed it.
//   - The existing (untouched, already-correct) implementation itself only
//     threads `Base(args)` through `TypeDecl::primaryBaseCtorArgs` on the
//     `hasPositionalFields` (primary-ctor) branch — including the
//     EMPTY-parens case `class D(): Base(99)`, which already worked before
//     this fix and still does (regression-guarded in
//     tests/codegen/inherit_header_ctor_args_no_primary_ctor_2455_test.cpp).
// So "legal without a primary constructor" would require inventing which of
// possibly several independently-`this(...)`-chaining explicit constructors
// the header argument list binds to — a question the spec never answers.
// Per CLAUDE.md rule 4, the simplest SOUND reading is REJECTION, recorded
// here and in the parser's own comment (Parser_Decls.cpp,
// `Parser::parseInheritance`).
//
// This file tests the parser-level diagnostic in isolation (fast, no
// codegen). End-to-end (full `bezelc -c` pipeline) coverage lives in the lit
// track: tests/lit/inherit_header_ctor_args_no_primary_ctor_2455_*.bzl.

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

// §11.1/§22.10/§12.8, v0.235: the issue's own confirmed repro, verbatim —
// `class LocalDerived: LocalBase(99)` with LocalDerived declaring no primary-
// constructor parameter list. Must be REJECTED, not silently accepted with
// the args dropped.
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           base_arglist_no_primary_ctor_rejected_spec_11_1_22_10_12_8) {
    auto r = parseAll(
        "module M;"
        "public class LocalBase"
        "  private _v: int;"
        "  public constructor(v: int) _v := v; end constructor;"
        "  public property Val: int => _v;"
        "end LocalBase;"
        "public class LocalDerived: LocalBase(99)"
        "end LocalDerived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "declares no primary-constructor parameter list of its own"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7" "11.1"));  // "§11.1"
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7" "22.10"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7" "12.8"));
}

// Same shape, generic class (axis: generic vs concrete) — GenericParams
// parses before Inheritance, so the rejection must still fire.
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           base_arglist_no_primary_ctor_generic_rejected_spec_11_1) {
    auto r = parseAll(
        "module M;"
        "public class Base<T> end Base;"
        "public class Derived<T>: Base<T>(1) end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "declares no primary-constructor parameter list of its own"));
}

// Same shape, abstract/sealed modifiers present (axis: modifier interaction).
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           base_arglist_no_primary_ctor_sealed_rejected_spec_11_1) {
    auto r = parseAll(
        "module M;"
        "public class Base end Base;"
        "public sealed class Derived: Base(1) end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "declares no primary-constructor parameter list of its own"));
}

// §A.3 InheritItem grammar note ("permitted only on the base class ... not
// on interfaces"), v0.235: an argument list on a NON-FIRST inheritance item
// (an interface position) is rejected with the distinct interface-specific
// message, for a class without a primary ctor.
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           interface_position_arglist_rejected_appendix_a) {
    auto r = parseAll(
        "module M;"
        "public interface IMarker end IMarker;"
        "public class Base end Base;"
        "public class Derived: Base, IMarker(1) end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an argument list is not permitted here"));
    BEZEL_EXPECT_TRUE(diagContains(r, "not on an interface"));
}

// --- Regression guards: legitimate shapes must NOT be rejected -----------

// Bare inheritance with no argument list at all — the overwhelmingly common
// case — must remain accepted (no false reject).
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           bare_inheritance_no_arglist_still_accepted) {
    auto r = parseAll(
        "module M;"
        "public class Base end Base;"
        "public class Derived: Base end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An EMPTY primary-constructor parameter list (`Derived()`) is a real
// primary constructor per §A.3's `PrimaryCtorParams ::= "(" ParamList? ")"`
// (ParamList is optional) — the header arg list is legal and must NOT be
// rejected. This is the "add an empty primary-constructor parameter list"
// remedy named in the new diagnostic; runtime-value regression coverage is
// in tests/codegen/inherit_header_ctor_args_no_primary_ctor_2455_test.cpp.
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           empty_primary_ctor_parens_arglist_still_accepted_spec_A_3) {
    auto r = parseAll(
        "module M;"
        "public class Base(v: int) end Base;"
        "public class Derived(): Base(99) end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A real (non-empty) primary constructor threading its own parameter to the
// base — the documented §11.1/§22.10 shorthand — must remain accepted.
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           real_primary_ctor_arglist_still_accepted_spec_11_1) {
    auto r = parseAll(
        "module M;"
        "public class Base(v: int) end Base;"
        "public class Derived(x: int): Base(x) end Derived;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Record inheritance (interfaces only) is untouched by this fix — an
// argument-list-free interface list still parses cleanly (out-of-scope
// nonsense-argument-list handling for records/interfaces is filed
// separately, not changed here).
BEZEL_TEST(parser_inherit_header_ctor_args_2455,
           record_interface_inheritance_unaffected_spec_9_6) {
    auto r = parseAll(
        "module M;"
        "public interface IMarker end IMarker;"
        "public record Point(X: int, Y: int): IMarker end Point;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

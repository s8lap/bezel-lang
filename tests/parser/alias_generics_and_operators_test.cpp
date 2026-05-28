// Spec-derived parser tests for the completed TypeAliasDecl (generic params
// + generic constraints, §A.2) and OperatorDecl (§A.4 §25).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

static bool succeedsParse(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

// --- TypeAlias generics + constraints --------------------------------------

BEZEL_TEST(parser_alias_op, generic_type_alias) {
    BEZEL_EXPECT_TRUE(
        succeedsParse("module M; type Box<T> = List<T>;"));
}

BEZEL_TEST(parser_alias_op, generic_type_alias_with_constraint) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; type DisposableList<T> = List<T> "
        "where T: IDisposable;"));
}

BEZEL_TEST(parser_alias_op, two_generic_params) {
    BEZEL_EXPECT_TRUE(
        succeedsParse("module M; type Pair<K, V> = Dictionary<K, V>;"));
}

// --- OperatorDecl ----------------------------------------------------------

BEZEL_TEST(parser_alias_op, operator_plus_expression_body) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Vec "
        "public static method operator +(a: Vec, b: Vec): Vec => a; "
        "end Vec;"));
}

BEZEL_TEST(parser_alias_op, operator_equals_expression_body) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; record Point(x: int, y: int) "
        "public static method operator =(a: Point, b: Point): bool "
        "  => a.x = b.x and a.y = b.y; "
        "end Point;"));
}

BEZEL_TEST(parser_alias_op, operator_with_block_body) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Box "
        "public static method operator -(a: Box): Box "
        "  return a; "
        "end; "
        "end Box;"));
}

BEZEL_TEST(parser_alias_op, operator_owned_return) {
    // §A.4: `owned` is permitted on operator return for class-typed
    // operators that allocate.
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; class Big "
        "public static method operator +(a: Big, b: Big): owned Big "
        "  => a; "
        "end Big;"));
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_alias_op, operator_without_return_type_is_error) {
    auto r = parseAll(
        "module M; class V "
        "public static method operator +(a: V, b: V) => a; "
        "end V;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

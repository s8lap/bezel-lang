// Spec-derived parser tests for constructors and destructors — §A.4
// ConstructorDecl and DestructorDecl.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// --- Instance constructor (A.4 first ConstructorDecl alternative) ----------

BEZEL_TEST(parser_ctor_dtor, instance_ctor_no_params_empty_body) {
    auto r = parseAll(
        "module M; class Foo public constructor() end constructor; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public constructor() {  }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ctor_dtor, instance_ctor_with_params_and_body) {
    auto r = parseAll(
        "module M; class Foo public constructor(name: string, age: int) "
        "  this.name := name; "
        "  this.age := age; "
        "end constructor; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public constructor(name: string, age: int) "
                "{ this.name := name; this.age := age; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §A.4 CtorChain: `base(args);` and `this(args);` parse as regular
// ExpressionStmt(CallExpr(this|base, args)) statements; sema folds the
// pattern into "ctor chain" semantics. The parser doesn't need to special-
// case them (avoids a `this.field` ambiguity at parse time).
BEZEL_TEST(parser_ctor_dtor, ctor_with_base_chain_parses_as_first_stmt) {
    auto r = parseAll(
        "module M; class Foo public constructor(x: int) "
        "  base(x); "
        "end constructor; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public constructor(x: int) { base(x); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ctor_dtor, ctor_with_this_chain_parses_as_first_stmt) {
    auto r = parseAll(
        "module M; class Foo public constructor() "
        "  this(0); "
        "end constructor; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public constructor() { this(0); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §1.6: optional `constructor` label after `end`.
BEZEL_TEST(parser_ctor_dtor, ctor_with_plain_end) {
    auto r = parseAll(
        "module M; class Foo public constructor(x: int) end; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public constructor(x: int) {  }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Static constructor (A.4 second ConstructorDecl alternative) -----------

BEZEL_TEST(parser_ctor_dtor, static_ctor_no_visibility) {
    auto r = parseAll(
        "module M; class Foo static constructor() x := 1; end constructor; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("static constructor() { x := 1; }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Destructor (A.4) ------------------------------------------------------

BEZEL_TEST(parser_ctor_dtor, destructor_simple) {
    auto r = parseAll(
        "module M; class Foo destructor cleanup(); end destructor; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("destructor { cleanup(); }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ctor_dtor, destructor_with_plain_end) {
    auto r = parseAll(
        "module M; class Foo destructor end; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("destructor {  }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_ctor_dtor, instance_ctor_without_visibility_is_error) {
    auto r = parseAll(
        "module M; class Foo constructor() end; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_ctor_dtor, destructor_with_visibility_is_error) {
    auto r = parseAll(
        "module M; class Foo public destructor end; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

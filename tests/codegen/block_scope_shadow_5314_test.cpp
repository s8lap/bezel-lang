// Regression tests for #5314, second root — sema left a local declared in an
// `if` / `elif` / `else` body, a `while` body or a `case` arm in the ENCLOSING
// scope, so after the block a read of a same-named parameter/field resolved to
// the inner local (wrong type: "argument ... not implicitly convertible", "no
// member `Length` on primitive type `int`"; and, for the definite-assignment
// pass, "read of unassigned local").
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §21.6 (v0.313): "Locals declared in a block
// are visible only until the matching `end`"; §21.1: a local's scope is "the
// innermost enclosing statement block"; §2.7 rule 1: a bare name resolves
// through local scope in lexical order, then the enclosing type's members. An
// `if`/`elif`/`else` body, a `while` body and a case arm are statement blocks,
// exactly like the `for` body and the `do ... end` block, which sema already
// scoped (#1242). So after the block `v` names the PARAMETER again.
//
// MEASURED PRE-FIX (staging r23-c28 14c07dfa + the #5314 DA commit): with
// `F(v: string)` and `if s = 0 then var v := 5; ... end; ... v.Length` sema
// reports "no member `Length` on primitive type `int`" and the DA pass "read of
// unassigned local `v`"; `for`/`do` already resolved correctly.
//
// Rows: if / elif / else / while / case arm, each over a STRING parameter
// shadowed by an `int` local (the type conflict makes a mis-resolution visible),
// plus an int-parameter `while` row (the DA-only shape from the issue), a field
// shadowed in an `if`, and sibling branches declaring the same name with
// different types.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `body` runs inside F(v: string) after `var s := 0;`; F returns s*1000 + the
// length of the parameter v.
std::string strProg(const std::string& body) {
    return "module M; public class Program "
           "  public static method F(v: string): int "
           "    var s := 0; " + body +
           "    return s * 1000 + v.Length; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(\"abcd\")); return 0; end Main; "
           "end Program;";
}

void expect(const std::string& src, const std::string& want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

BEZEL_TEST(block_scope_shadow_5314, if_body_shadows_param) {
    expect(strProg("if s = 0 then var v := 5; s := s + v; end; "), "5004\n");
}
BEZEL_TEST(block_scope_shadow_5314, elif_body_shadows_param) {
    expect(strProg("if s = 1 then s := 9; elif s = 0 then "
                   "  var v := 6; s := s + v; end; "),
           "6004\n");
}
BEZEL_TEST(block_scope_shadow_5314, else_body_shadows_param) {
    expect(strProg("if s = 1 then s := 9; else var v := 7; s := s + v; end; "),
           "7004\n");
}
BEZEL_TEST(block_scope_shadow_5314, while_body_shadows_param) {
    expect(strProg("var k := 0; while k < 3 do var v := k; s := s + v; "
                   "  k := k + 1; end; "),
           "3004\n");
}
BEZEL_TEST(block_scope_shadow_5314, case_arm_shadows_param) {
    expect(strProg("case s of 0 => do var v := 5; s := s + v; end, "
                   "_ => do end end; "),
           "5004\n");
}

// The issue's int-parameter shapes (DA-only symptom): F(3) = s*1000 + 3.
BEZEL_TEST(block_scope_shadow_5314, while_shadows_int_param_issue_shape) {
    expect("module M; public class Program "
           "  public static method F(v: int): int "
           "    var s := 0; var k := 0; "
           "    while k < 2 do var v := k; s := s + v; k := k + 1; end; "
           "    return s * 1000 + v; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(3)); return 0; end Main; "
           "end Program;",
           "1003\n");
}
BEZEL_TEST(block_scope_shadow_5314, if_shadows_int_param_issue_shape) {
    expect("module M; public class Program "
           "  public static method F(v: int): int "
           "    var s := 0; "
           "    if s = 0 then var v := 5; s := s + v; end; "
           "    return s * 1000 + v; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(3)); return 0; end Main; "
           "end Program;",
           "5003\n");
}

// A field `v` (a string) shadowed by an `if` body local of another type.
BEZEL_TEST(block_scope_shadow_5314, if_body_shadows_field) {
    expect("module M; public class Program "
           "  private v: string := \"hello\"; "
           "  public method F(): int "
           "    var s := 0; "
           "    if s = 0 then var v := 5; s := s + v; end; "
           "    return s * 1000 + v.Length; "
           "  end F; "
           "  public static method Main(): int "
           "    var p := new Program(); Console.WriteLine(p.F()); return 0; "
           "  end Main; "
           "end Program;",
           "5005\n");
}

// Sibling branches may each declare `t` with a different type; neither leaks.
BEZEL_TEST(block_scope_shadow_5314, sibling_branches_same_name_different_types) {
    expect("module M; public class Program "
           "  public static method F(t: string): int "
           "    var s := 0; "
           "    if s = 0 then var t := 5; s := s + t; "
           "    else var t := \"xy\"; s := s + t.Length; end; "
           "    return s * 1000 + t.Length; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(\"abc\")); return 0; end Main; "
           "end Program;",
           "5003\n");
}

// Regression tests for #5314 — a nested-block local that SHADOWS a parameter
// (or a field) made a later read of the parameter/field a false
// "read of unassigned local" (definite-assignment pass).
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §7.9 / §21.1 (v0.313): every read of a
// LOCAL must follow an assignment on every reaching path; a parameter is
// assigned on entry and a field is not a local. Spec §21.6 (v0.313): "Locals
// declared in a block are visible only until the matching `end`", and §2.7
// rule 1 resolves a bare name through local scope in lexical order, so after
// the block a read of `v` names the parameter (or field) again. §2.7 /
// §21.6 do not forbid the shadowing (the compiler accepts it for a `do` block
// and for a shadowed local).
//
// MEASURED PRE-FIX (staging r23-c28 14c07dfa): `for var i in 0..<2 do var v :=
// i; ... end; return s + v;` with parameter `v` -> "read of unassigned local
// `v`" at the return. Cause: the pass keys declared_/assigned_ by NAME; the
// nested `var v` enters both sets, the loop join drops it from assigned_ but
// not declared_, and the later read (resolved by sema to the PARAMETER) looked
// like an unassigned local. The read-site check now consults the resolved
// symbol: only a local (or an `out` parameter) is a tracked read.
//
// Rows here: the issue's `for` shape, a field shadowed in a `for`, and the
// controls (a `do` block, an outer local, a genuinely unassigned local).
// `while` / `if` / case-arm shapes also need the block-scope fix and live in
// block_scope_shadow_5314_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

void expect(const std::string& src, const std::string& want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// The issue's own repro: F(3) = (0 + 1) + 3.
BEZEL_TEST(da_shadow_by_symbol_5314, issue_repro_for_shadows_param) {
    expect("module M; public class Program "
           "  public static method F(v: int): int "
           "    var s := 0; "
           "    for var i in 0..<2 do var v := i; s := s + v; end; "
           "    return s + v; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(3)); return 0; end Main; "
           "end Program;",
           "4\n");
}

// A field `v` (100) shadowed by a `for` body local; the read after the loop is
// the field.
BEZEL_TEST(da_shadow_by_symbol_5314, for_shadows_field) {
    expect("module M; public class Program "
           "  private v: int := 100; "
           "  public method F(): int "
           "    var s := 0; "
           "    for var i in 0..<2 do var v := i; s := s + v; end; "
           "    return s + v; "
           "  end F; "
           "  public static method Main(): int "
           "    var p := new Program(); Console.WriteLine(p.F()); return 0; "
           "  end Main; "
           "end Program;",
           "101\n");
}

// The parameter's value (not the loop-local's last value) is what the read sees.
BEZEL_TEST(da_shadow_by_symbol_5314, for_shadows_param_string) {
    expect("module M; public class Program "
           "  public static method F(v: string): int "
           "    var s := 0; "
           "    for var i in 0..<3 do var v := i; s := s + v; end; "
           "    return s * 1000 + v.Length; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(\"abcd\")); return 0; end Main; "
           "end Program;",
           "3004\n");
}

// Control (already accepted pre-fix): a `do` block shadowing the parameter.
BEZEL_TEST(da_shadow_by_symbol_5314, do_block_shadows_param_control) {
    expect("module M; public class Program "
           "  public static method F(v: int): int "
           "    var s := 0; "
           "    do var v := 5; s := s + v; end; "
           "    return s * 1000 + v; "
           "  end F; "
           "  public static method Main(): int "
           "    Console.WriteLine(Program.F(3)); return 0; end Main; "
           "end Program;",
           "5003\n");
}

// Control: shadowing an outer LOCAL that has an initializer.
BEZEL_TEST(da_shadow_by_symbol_5314, for_shadows_initialized_local_control) {
    expect("module M; public class Program "
           "  public static method Main(): int "
           "    var v := 7; var s := 0; "
           "    for var i in 0..<2 do var v := i; s := s + v; end; "
           "    Console.WriteLine(s * 1000 + v); return 0; end Main; "
           "end Program;",
           "1007\n");
}

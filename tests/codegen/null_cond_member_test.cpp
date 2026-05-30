// Codegen slice CG41 — null-conditional member access `x?.M` (§7.5). If the
// target is null the result is null; otherwise the member is read. The
// result is always nullable, and `?.` short-circuits a chain. Reference-
// typed storage members only (value-typed results are deferred). Observable
// via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// N has a value field V and a self-referential nullable link `next`.
const char* kP =
    "module M; class N public V: int := 0; public next: N? := null; end N; "
    "class T public n: N? := null; end T; ";

} // namespace

BEZEL_TEST(codegen_cg41, member_present_yields_member) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t := new T(); var inner := new N(); inner.V := 7; t.n := inner; "
        "  var r := t?.n; return r!.V; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg41, null_target_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t: T? := null; var r := t?.n; "
        "  if r = null then return 5; end; return 0; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg41, null_member_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t := new T(); var r := t?.n; "
        "  if r = null then return 9; end; return 0; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_cg41, chain_intact) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a := new N(); var b := new N(); var c := new N(); c.V := 8; "
        "  a.next := b; b.next := c; var r := a?.next?.next; return r!.V; "
        "end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg41, chain_broken_in_middle_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a := new N(); var b := new N(); a.next := b; "
        "  var r := a?.next?.next; if r = null then return 4; end; return 0; "
        "end Main; end C;"), 4);
}

BEZEL_TEST(codegen_cg41, chain_first_link_null_short_circuits) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a: N? := null; var r := a?.next?.next; "
        "  if r = null then return 6; end; return 0; "
        "end Main; end C;"), 6);
}

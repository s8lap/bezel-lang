// #691 / Appendix A (v0.145): the grammar appendix caught up to prose the
// compiler already implements. Two forms are pinned here:
//   • §13.6 — the BARE single-statement setter `set => _x := value;` (an
//     AssignmentStmt, distinct from the `set => do … end` block form that
//     block_setter_test already covers, and from the `=> Expression` form —
//     assignment is a statement, not an expression).
//   • §23.3 — the interface PROPERTY accessor-block declaration form
//     (`property V: int get; set; end;`), complementing the interface-indexer
//     coverage in iface_indexer_owned_setter_681_test.
// Appendix A previously admitted neither; v0.145 added `AssignmentStmt` to
// AccessorBody and the accessor-block alternative to IfaceProperty/IfaceIndexer.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §13.6 / #691: the bare statement-body setter `set => _x := value;` stores
// through the assignment.
BEZEL_TEST(accessor_grammar_691, statement_body_setter_bare) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => _x := value; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 42; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

// §13.6 / #691: the bare statement-body setter with a renamed parameter
// (`set(v) => _x := v + 1;`) — SetterParamSpec + AssignmentStmt.
BEZEL_TEST(accessor_grammar_691, renamed_param_statement_setter) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set(v) => _x := v + 1; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 7; return c.P; "
        "  end Main; "
        "end C;"), 8);
}

// §23.3 / #691: an interface PROPERTY declared with the accessor-block form
// (`get; set; end;`) is implemented by a class (whose own property uses the
// bare statement setter) and dispatched through the interface.
BEZEL_TEST(accessor_grammar_691, interface_property_accessor_block) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public interface IBox "
        "  property V: int get; set; end; "
        "end IBox; "
        "class Box: IBox "
        "  public _v: int := 0; "
        "  public property V: int get => _v; set => _v := value; end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); "
        "  b.V := 9; "
        "  return b.V; "
        "end Main; end C;"), 9);
}

// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, try_return_catch_rethrow_ok).
//
// #1063 / §21.7 / §31.2.2: a try whose body returns and whose catch arm is an
// EXPRESSION-form throw (`catch E => throw;` rethrow, or `catch E => throw X`)
// always exits — the throw-expression diverges. Before the fix these were
// parsed as an ExpressionStmt wrapping a ThrowExpr (not a ThrowStmt), which
// stmtAlwaysExits did not recognize, so they were wrongly flagged "does not
// return a value on every path."
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(): int try return 1; catch ArgumentException => throw; end; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

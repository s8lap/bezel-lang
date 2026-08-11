// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// §31.4 / §31.2.1 (v0.243, proposal 0080, #3046) — the one residual carve, and
// the boundary the new arm-type rule must NOT cross.
//
// §31.4 declares `StackOverflowException` in the built-in hierarchy under
// `Exception` ("call stack exhausted; cannot be caught; terminates the
// program"), and §31.2.3 names it as the sole exception to the bare-`Exception`
// catch-all: "the sole exception is `StackOverflowException`, which no arm
// matches — [§31.4]".
//
// That is a LIVENESS fact, not a TYPING one (the v0.243 changelog states it in
// exactly those words: "the sole residual carve is the uncatchable
// `StackOverflowException`, §31.4 — a liveness fact, not a typing one"). So the
// §31.2.1 arm-type rule must leave a `StackOverflowException` arm ALONE: it IS
// `Exception`-derived, so it is neither the provably-dead case nor the `Object`
// case, and the new check must neither reject it nor double-report it through
// some second "uncatchable" path.
//
// Clean-compile ACCEPT control: `bezelc -c` must exit zero and emit no
// diagnostics.

module M3046F;

class C
  public static method Main(): int
    try
      throw new ArgumentException("x");
    catch
      StackOverflowException => do return 1; end,
      var s: StackOverflowException | OverflowException => do return 2; end,
      var e: Exception => do return 3; end
    end;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: is not an exception type
// NODIAG-NOT: §

// Codegen — issue #7611: a class instance whose function-typed FIELD holds a
// lambda that captures a `for` variable, or a parameter, leaked one capture
// cell per iteration (the filed repro: 400 B in 100 allocations over 100
// iterations, 4 B per captured `int`).
//
// THE RULE. §27.5 (v0.359, "Closures"): "Captured variables are heap-allocated
// in a closure object", and "For `for` loop variables, each iteration produces
// a fresh binding. A lambda captured inside the body sees that iteration's
// value." §28.9 (v0.359, "Runtime-managed types"): a function value, capturing
// or not, is runtime-managed — "the type's lifetime is managed by the runtime
// without user involvement" — so once the closure and the frame that captured
// into it are both gone, nothing of it may remain, and while either is alive
// the captured variable must still read correctly.
//
// ROOT CAUSE. Neither the instance nor the closure leaked: `H$DisposeCore`
// releases the field's closure. The leaked block is the CAPTURE CELL. A lambda
// stored into a field (`new H(lambda ...)`, `h.Cb := lambda ...`) is an
// escaping capturer that is not DEFINITELY escaping, so #694's and #3426's
// envdtor frees decline it and the scope-end free (#429 / #793 for a for-var)
// declines it too. #4509's refcounted cell (`bz_cell_retain` at each env's
// capture-store, `bz_cell_release` in its envdtor and once for the declaring
// frame) is the route for exactly that set, but it excluded the for-var and a
// captured parameter: `declScopeDepth_` records neither, so the frame's
// release had no scope to go in. MEASURED on 6d709c00f: the envdtor loads the
// cell and does nothing.
//
// THE FIX. `cellOwnerScope_` gives both a declaring scope. lowerForBody opens
// a per-iteration scope for an escaping-captured for-var's fresh cell (§27.5's
// fresh binding is one cell per iteration, so the frame's reference dies with
// the iteration); a parameter's first capture records its body's root scope.
// lowerLambda then arms the cell exactly as for a captured local.
//
// Every program loops 100 times; the leak rows run under `runAotChecked`
// (AddressSanitizer + LeakSanitizer) and compare the full stdout.
//
// MEASURED (`bezelc --sanitize=address`, detect_leaks=1):
//   row                                         6d709c00f          with the fix
//   issue_repro_forvar_ctor_field_spec_27_5     400 B / 100 leak   clean, 100
//   forvar_ctor_field_called_spec_27_5          400 B / 100 leak   clean, 5050
//   forvar_later_assigned_field_spec_27_5       400 B / 100 leak   clean, 5050
//   param_ctor_field_spec_28_9                  400 B / 100 leak   clean, 5050
//   param_later_assigned_field_spec_28_9        400 B / 100 leak   clean, 5050
//   (the remaining rows' base column is in the commit body)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The holder classes every row shares: `H` takes the closure in its
// constructor, `G` has a nullable field assigned after construction.
const char* kHolders = R"BZL(
module M;
class H(c: (int) -> int)
  public Cb: (int) -> int := c;
end H;
class G
  public Cb: ((int) -> int)?;
end G;
)BZL";

std::string prog(const std::string& body) { return kHolders + body; }

// stdout of a checked run (ASan + LSan) that compiled, ran and exited 0, or a
// marker naming what failed.
std::string checkedOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// #7611's repro as filed (field_nointerp.bzl): a for-var captured by a lambda
// a constructor stores into a nullable field.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           issue_repro_forvar_ctor_field_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
class H(c: ((int) -> int)?)
  public Cb: ((int) -> int)? := c;
  public method Get(): ((int) -> int)?
    return Cb;
  end Get;
end H;
class C
  static method MkF(n: int): ((int) -> int)?
    if n mod 2 = 0 then return null; end;
    return lambda(x: int) => x + n;
  end MkF;
  static method MkG(n: int): (int) -> int
    return lambda(x: int) => x + n;
  end MkG;
  public static method Main(): int
    var ok := 0;
    for var i in 0..<100 do
      var a: ((int) -> int)? := lambda(x: int) => x + i; var h := new H(a); var h0 := new H(null); ok := ok + 1;
    end;
    Console.WriteLine(ok);
    return 0;
  end Main;
end C;
)BZL"),
              "100\n");
}

// The closure is called through the field, so each iteration's own `i` is
// read back: sum of (1 + i) over 0..99 = 5050.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           forvar_ctor_field_called_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      var h := new H(lambda(x: int) => x + i);
      s := s + h.Cb(1);
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL")),
              "5050\n");
}

BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           forvar_later_assigned_field_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      var g := new G();
      g.Cb := lambda(x: int) => x + i;
      var f := g.Cb;
      if f is not null then s := s + f(1); end;
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL")),
              "5050\n");
}

// A parameter captured by a lambda the constructor stores: one cell per CALL.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           param_ctor_field_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method F(i: int): int
    var h := new H(lambda(x: int) => x + i);
    return h.Cb(1);
  end F;
  public static method Main(): int
    var s := 0;
    for var k in 0..<100 do
      s := s + C.F(k);
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL")),
              "5050\n");
}

BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           param_later_assigned_field_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method F(i: int): int
    var g := new G();
    g.Cb := lambda(x: int) => x + i;
    var f := g.Cb;
    if f is not null then return f(1); end;
    return 0;
  end F;
  public static method Main(): int
    var s := 0;
    for var k in 0..<100 do
      s := s + C.F(k);
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL")),
              "5050\n");
}

// The closure OUTLIVES its iteration: each inner iteration overwrites `g.Cb`
// (releasing the previous closure), and the last one is called after the
// inner loop has ended, so it must still read its own `i` (9) — the frame's
// per-iteration release must not free a cell an env still holds. Per outer
// `r`: 1 + 9 + r; summed over 0..99: 1000 + 4950 = 5950.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           forvar_closure_outlives_its_iteration_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  public static method Main(): int
    var t := 0;
    for var r in 0..<100 do
      var g := new G();
      for var i in 0..<10 do
        g.Cb := lambda(x: int) => x + i + r;
      end;
      var f := g.Cb;
      if f is not null then t := t + f(1); end;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "5950\n");
}

// Every exit from the body releases the frame's reference once: `continue`,
// `return` from inside the loop, and normal completion. Run(60) returns at
// i = 50; Run(10) completes.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           forvar_continue_and_return_exits_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method Run(n: int): int
    var s := 0;
    for var i in 0..<n do
      var h := new H(lambda(x: int) => x + i);
      if i mod 3 = 0 then s := s + h.Cb(0); continue; end;
      if i = 50 then return s + h.Cb(1000); end;
      s := s + 1;
    end;
    return s;
  end Run;
  public static method Main(): int
    var t := 0;
    for var k in 0..<100 do
      t := t + C.Run(60) + C.Run(10);
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "151500\n");
}

// The unwind edge: a throw out of the body after the capture.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           forvar_throw_out_of_body_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method Run(n: int): int
    var s := 0;
    for var i in 0..<n do
      var h := new H(lambda(x: int) => x + i);
      s := s + h.Cb(1);
      if i = 7 then throw new InvalidOperationException("x"); end;
    end;
    return s;
  end Run;
  public static method Main(): int
    var t := 0;
    for var k in 0..<100 do
      try
        t := t + C.Run(20);
      catch
        InvalidOperationException => do
          t := t + 1;
        end
      end try;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "100\n");
}

// A parameter captured inside a loop: ONE cell per call (guarded boxing),
// held by five envs; the frame releases it once, at the body's end. Per call
// sum over k of (1 + i + k) = 5 + 5i + 10; summed over i in 0..99: 26250.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           param_captured_inside_loop_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method F(i: int): int
    var s := 0;
    for var k in 0..<5 do
      var h := new H(lambda(x: int) => x + i + k);
      s := s + h.Cb(1);
    end;
    return s;
  end F;
  public static method Main(): int
    var t := 0;
    for var k in 0..<100 do t := t + C.F(k); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "26250\n");
}

// §27.5: "Modifications to a captured variable inside the lambda are visible
// outside, and vice versa" — the frame writes the parameter through the cell
// after the capture, and the closure reads the write: 1 + i + 10.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           param_written_after_capture_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  static method F(i: int): int
    var h := new H(lambda(x: int) => x + i);
    i := i + 10;
    return h.Cb(1);
  end F;
  public static method Main(): int
    var t := 0;
    for var k in 0..<100 do t := t + C.F(k); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "6050\n");
}

// A LAMBDA's own parameter, captured by a lambda nested in it and stored:
// the declaring body is the outer lambda's.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           lambda_param_captured_by_nested_stored_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(prog(R"BZL(
class C
  public static method Main(): int
    var mk: (int) -> int := lambda(a: int) => do
      var h := new H(lambda(x: int) => x + a);
      return h.Cb(1);
    end;
    var t := 0;
    for var k in 0..<100 do t := t + mk(k); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL")),
              "5050\n");
}

// BOUNDARY CONTROL (lane contract §7; green on 6d709c00f and with the fix):
// the closures escape the loop into an `Array`, which keeps the pointer
// without taking a reference (#5866), and are called AFTER the loop; each must
// print its own iteration's value (§27.5's fresh binding). The stored closures
// leak by #5866, so the oracle is `runAotCheckedLenient` (AddressSanitizer on,
// LeakSanitizer off). It goes RED (heap-use-after-free) under the plausible
// wrong fix of FREEING each iteration's cell at the body's end — arming
// lowerForBody's #793 `boxedCellFree` for an escaping capture — instead of
// releasing the frame's counted reference.
BEZEL_TEST(closure_cell_forvar_param_escaping_7611,
           control_forvar_closures_escape_loop_into_array_spec_27_5) {
    auto r = runAotCheckedLenient(R"BZL(
module M;
class C
  public static method Main(): int
    var fs: Array<(int) -> int> := new Array<(int) -> int>();
    for var i in 0..<100 do
      fs.Add(lambda(x: int) => x + i);
    end;
    var s := 0;
    for var j in 0..<100 do
      s := s + fs[j](1000) * (j + 1);
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5383300\n"));
}

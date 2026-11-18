// Codegen / §27.5 / §28.9 — issue #8765: a GENERIC class constructor (and
// the RECORD twin) whose body forms a lambda local that CAPTURES A
// CONSTRUCTOR PARAMETER leaked the 4 B capture cell once per construction.
//
// THE SPEC. §27.5 "Closures" (v0.359): "A lambda may reference variables
// from its enclosing scope. Captured variables are heap-allocated in a
// closure object, a `ref` or `out` parameter excepted (below)." §28.9
// "Runtime-managed types" (v0.359): "the type's lifetime is managed by the
// runtime without user involvement" — so once the closure and the frame that
// captured into it are both gone, nothing of it may remain. The captured
// variable's heap cell is part of that: §27.5 heap-allocates it, so exactly
// one reclamation is owed, at the end of the scope that declares it.
//
// THE DEFECT. A generic class constructor is INLINED into the CALLER's frame
// (`needsInlineForLayout` -> `inlineCtorBody`, src/codegen/LlvmBackend_Setup.cpp),
// and the constructor body opens its OWN scope there. #3403's guard in
// `Emitter::consumeLastLambdaCapturedCells` reads "a parameter is bound in
// the body's OUTERMOST scope" as "disposeStack_.size() == 1" — the
// function-body ROOT scope. That is true of a COMPILED constructor (whose
// body scope IS the root scope) and false of an INLINED one, whose
// parameters are declared in the constructor body's own scope, one level
// deeper. So the guard declined the capture-cell free for EVERY parameter
// captured by an inlined constructor body, and the 4 B cell leaked once per
// construction.
//
// MEASURED (bezelc --sanitize=address, detect_leaks=1), 200 constructions:
//   program                                     base          with the fix
//   generic class ctor, captured parameter      800 B / 200   clean, 20100
//   record ctor, captured parameter              800 B / 200   clean, 20100
//   generic class ctor, TWO lambdas on it       800 B / 200   clean, 40400
//   generic class ctor, REASSIGNED parameter    800 B / 200   clean, 22100
//   (the issue's own repro loops 50 times; the base then leaks 200 B / 50)
//
// The issue's note that "the record form is clean" does NOT hold for a
// record with an explicit constructor whose body captures a constructor
// parameter: measured on the base tree, the record twin leaks the same
// 800 B / 200. The two shapes share one root cause (the record constructor's
// inline path is the record twin of `inlineCtorBody`), so they are fixed
// together.
//
// Every row loops 200 times (a one-shot leak hides in sanitizer noise) and
// runs under `runAotChecked` (checked disposal + generated-code ASan +
// LeakSanitizer). Values go to stdout (exit codes are 8-bit truncated).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The stdout of a checked run (ASan + LSan) that compiled, ran and exited 0,
// or a marker naming what failed. A leak makes the run exit non-zero, so the
// marker differs from the expected output and the row fails.
std::string checkedOut(const std::string& program) {
    JitResult r = runAotChecked(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// The stdout of a run under generated-code ASan with LeakSanitizer OFF
// (runAotCheckedLenient), or a marker naming what failed. For the two
// nested-body rows below: their subject is a use-after-free (a null cell
// read), not a leak — the cell of a parameter captured in a NESTED block of
// its own body and read after that block is DELIBERATELY not reclaimed there
// (#3403's guard, the bounded leak #3403's rule accepts; the non-generic
// twin of the same program leaks the same 800 B / 200 on the base and with
// this fix), so the memory net that decides them is ASan, not LSan.
std::string asanOut(const std::string& program) {
    JitResult r = runAotCheckedLenient(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's literal repro, loop-amplified: a GENERIC class whose
// constructor forms a lambda local capturing the constructor's `n`
// parameter. Sum of (i + 1) over i in 0..<200 = 20100.
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           issue_repro_generic_ctor_param_capture_cell_released_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
public class G<T>
  public v: int;
  public constructor(x: T, n: int)
    var g := lambda() => n + 1;
    v := g();
  end constructor;
end G;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0;
    var tot := 0;
    while i < 200 do
      using var h := new G<string>("a", i);
      tot := tot + h.v;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
)BZL"),
              "20100\n");
}

// The RECORD twin of the issue's repro: the record constructor's inline path
// (lowerNew's record-ctor inline, the record twin of `inlineCtorBody`) had
// the same 4 B capture cell leaked once per construction — the issue's note
// that the record form is clean does not hold for this shape (measured on
// the base tree: 800 B in 200 allocations over 200 constructions).
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           record_ctor_param_capture_cell_released_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
public record G
  public v: int;
  public constructor(x: string, n: int)
    var g := lambda() => n + 1;
    v := g();
  end constructor;
end G;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0;
    var tot := 0;
    while i < 200 do
      var h := new G("a", i);
      tot := tot + h.v;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
)BZL"),
              "20100\n");
}

// TWO lambdas capture the same parameter: one cell, shared by two envs, and
// the frame's reference is released once. Sum of (2i + 3) over i in 0..<200
// = 40400. A fix that freed the cell once PER CAPTURE would double-free it
// here; a fix that freed it once per CONSTRUCTION is what this pins.
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           two_lambdas_sharing_the_param_cell_released_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
public class G<T>
  public v: int;
  public constructor(x: T, n: int)
    var g1 := lambda() => n + 1;
    var g2 := lambda() => n + 2;
    v := g1() + g2();
  end constructor;
end G;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0;
    var tot := 0;
    while i < 200 do
      using var h := new G<string>("a", i);
      tot := tot + h.v;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
)BZL"),
              "40400\n");
}

// The parameter is REASSIGNED after the capture: §27.5 "Modifications to a
// captured variable inside the lambda are visible outside, and vice versa" —
// the closure must read the value the frame wrote through the shared cell,
// and the cell must still be reclaimed exactly once. The frame reads the
// reassigned value: (i + 10) + 1 = i + 11, summed over i in 0..<200 =
// 22100.
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           reassigned_param_capture_cell_released_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
public class G<T>
  public v: int;
  public constructor(x: T, n: int)
    var g := lambda() => n + 1;
    n := n + 10;
    v := g();
  end constructor;
end G;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0;
    var tot := 0;
    while i < 200 do
      using var h := new G<string>("a", i);
      tot := tot + h.v;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
)BZL"),
              "22100\n");
}

// fix-forward (review r42-rev-m78 probes b12 / b15): the depth threshold the
// fix above sets for an inlined constructor body must not leak into a
// DIFFERENT body lowered inside it. A block lambda formed in a GENERIC
// constructor body is lowered into its own function with a fresh
// `disposeStack_` (its root scope is depth 1 again), so with the
// constructor's threshold (>= 2) inherited, #3403's guard admitted the free
// of the LAMBDA's own parameter `m`'s cell at the nested `do` block's end,
// nulled the holder, and `return s + m` read through null: an ASan SEGV
// (READ of address 0) in `lam$0`, with no output. The threshold is now
// honoured only in the body it was set for (`currentFn_`). §27.5 (v0.359):
// "Captured variables are heap-allocated in a closure object" — the lambda
// reads its own captured `m`, so s = m and each construction adds 2i:
// 2 * 19900 = 39800.
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           lambda_in_generic_ctor_own_param_captured_in_nested_block_read_after_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(asanOut(R"BZL(
module T;
public class G<T>
  public v: int;
  public constructor(x: T, n: int)
    var outer := lambda(m: int): int => do
      var s := 0;
      do
        var g := lambda(): int => m;
        s := g();
      end;
      return s + m;
    end;
    v := outer(n);
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var tot := 0;
    while i < 200 do using var h := new G<string>("a", i); tot := tot + h.v; i := i + 1; end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
              "39800");
}

// The same hazard two blocks deep in the lambda, with the generic
// constructor itself constructed two blocks deep in the caller's loop body
// (so the inherited threshold was 5): outer(n) = (n + 1) + n and
// outer(n + 1) = (n + 2) + (n + 1), so each construction adds 4i + 4:
// 4 * 19900 + 800 = 80400.
BEZEL_TEST(codegen_inline_ctor_param_capture_cell_release_8765,
           lambda_in_generic_ctor_own_param_captured_two_blocks_deep_caller_nested_200x_spec_27_5) {
    BEZEL_EXPECT_EQ(asanOut(R"BZL(
module T;
public class G<T>
  public v: int;
  public constructor(x: T, n: int)
    var outer := lambda(m: int): int => do
      var s := 0;
      do
        do
          var g := lambda(): int => m + 1;
          s := g();
        end;
      end;
      return s + m;
    end;
    v := outer(n) + outer(n + 1);
  end;
end;
public static class Program
  public static method Main(args: Array<string>): int
    var i := 0; var tot := 0;
    while i < 200 do
      do
        do
          using var h := new G<string>("a", i); tot := tot + h.v;
        end;
      end;
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
)BZL"),
              "80400");
}

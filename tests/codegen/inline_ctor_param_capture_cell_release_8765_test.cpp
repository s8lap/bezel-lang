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

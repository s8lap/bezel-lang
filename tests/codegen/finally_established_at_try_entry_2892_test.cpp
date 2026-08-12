// fix #2892 — a `try` statement's own `finally` is ESTABLISHED AT ENTRY TO THE
// PROTECTED BLOCK, so a body-declared owned local / `using var` is disposed
// BEFORE the finally on every exit edge.
//
// WHAT THE DOCUMENT REQUIRES.  §28.10 `using var` (v0.252):
//
//     A `try` statement's own `finally` is **established at entry to the
//     protected block** — before any establishment the block itself performs.
//     Consequently, on every exit edge, an owned local or `using var` declared
//     **inside** the protected block (established later) is disposed **before**
//     the `finally` runs, exactly as the desugared nested `try`/`finally` of this
//     section and §29.6 would dispose it; cleanups established **before** the
//     `try` run after the `finally`, as before (§31.2).
//
// §31.2 `try`/`catch`/`finally` (v0.252) states the same rule:
//
//     The `finally` is **established at entry to the protected block** (§28.10):
//     on the single establishment-ordered cleanup stack it ranks before anything
//     the block itself establishes, so an owned local or `using var` declared
//     inside the protected block is disposed before the `finally` runs on every
//     exit edge, and cleanups established before the `try` run after it.
//
// and the ranking is the SAME on every edge, because §28.10 also says "Cleanup
// ordering is uniform across exit edges: ... on *every* exit edge (normal,
// `return`, `break`/`continue`, exception, and an iterator's early disposal,
// §29.7), pending cleanups run in reverse order of establishment. No edge runs
// the two kinds in separate category-ordered phases."
//
// WHAT THIS FILE REPLACES.  It is the re-derivation of
// `finally_before_autodispose_1263_test.cpp`, which pinned the OPPOSITE order —
// the finally first, then the body locals — quoting a §28.3 sentence
// ("the explicit `finally` runs first (with the local still in scope), then the
// implicit auto-dispose runs as the scope unwinds further") that no longer
// exists: v0.240 replaced it with the establishment-conditional form, and #2892
// asked which establishment point a `try`'s own `finally` has.  The owner ruled
// (v0.241, PR #3043) for entry-to-the-protected-block — Reading A — and recorded
// why the textual-position reading was not codifiable: it admits no
// establishment-point formulation at all (ranking a finally at its clause's
// position inverts nesting for nested finallys) and keeping it would require a
// category-ordered phase, the construct v0.182 abolished.
//
// WHAT MEASURABLY CHANGED (each value run on 663a68e before the fix and after
// it, same program, both ASan/LSan-clean):
//   normal fall-through   9421 -> 9214      exception (outer catch)  4217 -> 2147
//   `using var` body      4217 -> 2147      catch+finally, one try   7421 -> 2174
//   nested, normal       95261 -> 92516     nested, exception       52617 -> 25167
//   three-level exc.   9382716 -> 3928176   throw from the finally    417 -> 147
//   enclosing+body local  9421 -> 9241      `owned` param + body      9421 -> 9241
//   unmatched arm         4217 -> 2147      arm `return`             7214 -> 2174
//   async normal          9421 -> 9214      async exception          4217 -> 2147
//   `when` guard throws    417 -> 147       arm re-raise             8417 -> 1847
//   break to inner loop     941 -> 914
// and what did NOT change, because those edges already ranked the finally at
// protected-block entry: `return` 214, `break` 214, `continue` 214214, async
// `return` 214, nested `return` 2516, iterator early dispose 24, `yield break`
// 24, labeled `break` 3921, the enclosing-`using var` orderings of
// #1567/#1598/#2051 (147), and a try with a catch and NO finally (17 — the only
// lowerTry path this fix does not touch).  The fix is therefore also what makes
// §28.10's "uniform across exit edges" true of this compiler: before it, the
// direct edges disagreed with each other.
//
// OBSERVATION TECHNIQUE.  A static `_seq` counter each cleanup action folds a
// digit into (`_seq := _seq * 10 + tag`); the final value pins the exact order.
// A wrong order, a missed disposal and a double dispose all produce different
// values.  Everything runs under generated-code ASan/LSan (`runAotChecked`), so
// a leak / double-free / use-after-free fails the test rather than shifting a
// digit.  Sequences beyond the 8-bit exit-code ceiling travel on stdout
// (`checkedOut` below) — same ASan/LSan net, different channel.
//
// (AOT MIGRATION: the #1263 file ran on a file-local in-process LLJIT runner.
// This one is on the real `bezelc` AOT driver, the strict oracle.)

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Ordering oracle + memory net: ASan/LSan on the generated program, so a
// dispose-twice / use-after-dispose is `ran=false` (-999) rather than a digit.
int checked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The same net for sequences that exceed 255: the program prints the counter
// and the expectation is the bare value (a trailing line-ending is stripped,
// exactly as runAotOut does — but runAotOut is plain runAot, and this file needs
// the sanitizer).
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

}  // namespace

// ===========================================================================
// THE RULE ITSELF — one body-declared cleanup, every exit edge of one `try`.
// ===========================================================================

// §28.10 (v0.252): NORMAL fall-through.  `r1`/`r2` are established at their
// declarations, i.e. AFTER the finally (which is established at protected-block
// entry), so reverse-establishment disposes r2 then r1 and only then runs the
// finally: body marker 9, r2=2, r1=1, finally=4 -> 9214.
// Pre-#2892 this was 9421 (the finally ran first).
BEZEL_TEST(finally_established_at_try_entry_2892,
           normal_edge_body_locals_dispose_before_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      var r1 := new R1();
      var r2 := new R2();
      C._seq := C._seq * 10 + 9;
    finally
      C._seq := C._seq * 10 + 4;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "9214");
}

// §28.10 (v0.252): EXCEPTION edge.  The throw leaves the protected block, so the
// body's owned locals are disposed during that unwinding (§28.3 exception-path
// discharge) — before the finally, which is established at protected-block entry
// and therefore ranks below them: r2=2, r1=1, finally=4, outer catch=7 -> 2147.
// Pre-#2892 this was 4217.  This is #1263's own repro; its pinned 421 (the seq
// without the outer catch's digit) is the value the ruling names as becoming 214.
BEZEL_TEST(finally_established_at_try_entry_2892,
           exception_edge_body_locals_dispose_before_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        var r2 := new R2();
        throw new E();
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2147");
}

// §28.10 (v0.252): the rule is stated over "an owned local **or `using var`**",
// so the explicit spelling orders identically to the §28.3 auto-drop one: 2147.
// Pre-#2892 this was 4217.
BEZEL_TEST(finally_established_at_try_entry_2892,
           exception_edge_using_var_disposes_before_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        using var r1 := new R1();
        using var r2 := new R2();
        throw new E();
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2147");
}

// §28.10 / §31.2 (v0.252): one `try` with BOTH a matching catch arm and a
// finally.  The body is still left by the throw, so its locals are disposed
// first; the arm handles the exception; the finally runs last: r2=2, r1=1,
// catch=7, finally=4 -> 2174.  Pre-#2892 this was 7421.
BEZEL_TEST(finally_established_at_try_entry_2892,
           catch_arm_and_finally_on_one_try_spec_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      var r1 := new R1();
      var r2 := new R2();
      throw new E();
    catch E => C._seq := C._seq * 10 + 7;
    finally
      C._seq := C._seq * 10 + 4;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2174");
}

// §31.2 (v0.252): an UNMATCHED arm — "If no arm matches, the exception continues
// propagating to the enclosing handler" — and the finally still runs.  Same
// ordering as the no-catch exception edge: 2147.  Pre-#2892 this was 4217.
BEZEL_TEST(finally_established_at_try_entry_2892,
           unmatched_arm_repropagation_spec_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class F: Exception end F;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        var r2 := new R2();
        throw new E();
      catch F => C._seq := C._seq * 10 + 8;
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2147");
}

// §31.2 (v0.252): "`finally` runs whether the protected block completed
// normally, was exited by an exception (caught or uncaught), or was exited by
// `return`".  A `return` from the protected block: r2=2, r1=1, finally=4 -> 214.
// UNCHANGED by #2892 — this edge already disposed the body scope at the exit
// site, i.e. already ranked the finally at protected-block entry.
BEZEL_TEST(finally_established_at_try_entry_2892,
           return_edge_unchanged_spec_31_2) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    try
      var r1 := new R1();
      var r2 := new R2();
      return 0;
    finally
      C._seq := C._seq * 10 + 4;
    end;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    return C._seq;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), 214);
}

// §31.2 (v0.252): "... or by a `break` or `continue` targeting an enclosing
// loop."  UNCHANGED by #2892 (214), and now IDENTICAL to the normal and
// exception edges' relative order — which is §28.10's "uniform across exit
// edges" made true.
BEZEL_TEST(finally_established_at_try_entry_2892,
           break_edge_unchanged_spec_31_2) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    for var i in 0..3 do
      try
        var r1 := new R1();
        var r2 := new R2();
        break;
      finally
        C._seq := C._seq * 10 + 4;
      end;
    end;
    return C._seq;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), 214);
}

// §31.2 (v0.252): the `continue` twin, run for two iterations (214 214).
// UNCHANGED by #2892.  Travels on stdout (214214 > 255).
BEZEL_TEST(finally_established_at_try_entry_2892,
           continue_edge_unchanged_spec_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    for var i in 0..1 do
      try
        var r1 := new R1();
        var r2 := new R2();
        continue;
      finally
        C._seq := C._seq * 10 + 4;
      end;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "214214");
}

// §28.3 / §31.2 (v0.252): a matching catch arm that RETURNS.  The body local was
// disposed on the way out of the protected block, the arm runs, then the
// finally: r2=2, r1=1, catch=7, finally=4 -> 2174.  Pre-#2892 this was 7214 (the
// arm ran first and the body scope was disposed at the arm's return site).
BEZEL_TEST(finally_established_at_try_entry_2892,
           catch_arm_return_edge_spec_31_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    try
      var r1 := new R1();
      var r2 := new R2();
      throw new E();
    catch E => do
      C._seq := C._seq * 10 + 7;
      return 0;
    end
    finally
      C._seq := C._seq * 10 + 4;
    end;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2174");
}

// ===========================================================================
// THE OTHER HALF OF THE RULE — "cleanups established BEFORE the `try` run after
// the `finally`".  These must NOT move; they are the #1567/#1598/#2051 family.
// ===========================================================================

// §28.10 (v0.252) regression guard: an ENCLOSING `using var a` established
// before the try, whose field the finally reads.  `a` is established BEFORE the
// finally, so it is disposed AFTER it — otherwise the read is a use-after-
// dispose (which ASan would fail here, not merely reorder a digit).
// a.tag=1, a's destructor=4, outer catch=7 -> 147.  UNCHANGED by #2892.
BEZEL_TEST(finally_established_at_try_entry_2892,
           enclosing_using_var_disposes_after_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      using var a := new A();
      try
        throw new E();
      finally
        C._seq := C._seq * 10 + a.tag;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    return C._seq;
  end Main;
end C;
class A
  public tag: int := 1;
  destructor C._seq := C._seq * 10 + 4; end destructor;
end A;
)"), 147);
}

// §28.10 (v0.252): the MIXED shape that separates the two halves of the rule in
// one program — an enclosing `using var e` (established before the try) and a
// body-declared `using var b` (established after the finally).  Reverse
// establishment: body marker 9, b=2, finally=4, e=1 -> 9241.  Pre-#2892 this was
// 9421: `b` was disposed after the finally, i.e. it was ranked as if it had been
// established before the try, exactly like `e`.
BEZEL_TEST(finally_established_at_try_entry_2892,
           enclosing_and_body_cleanups_straddle_the_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    do
      using var e := new Ee();
      try
        using var b := new B();
        C._seq := C._seq * 10 + 9;
      finally
        C._seq := C._seq * 10 + 4;
      end;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class Ee destructor C._seq := C._seq * 10 + 1; end destructor; end Ee;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), "9241");
}

// §28.3 (v0.252): "Undischarged `owned` parameters' implicit disposals are
// established at entry to the body" — i.e. before a `try` written inside that
// body, hence disposed AFTER its finally; a body-declared local is disposed
// before it.  body marker 9, b=2, finally=4, owned param a=1 -> 9241.
// Pre-#2892 this was 9421.
BEZEL_TEST(finally_established_at_try_entry_2892,
           owned_parameter_disposes_after_finally_spec_28_3) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Take(owned p: A): int
    try
      var b := new B();
      C._seq := C._seq * 10 + 9;
    finally
      C._seq := C._seq * 10 + 4;
    end;
    return 0;
  end Take;
  public static method Main(): int
    C._seq := 0;
    _ := Take(new A());
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), "9241");
}

// ===========================================================================
// NESTED `try`/`finally` — the shape the ruling's argument turns on.  Ranking a
// finally at its clause's TEXTUAL position inverts nesting; ranking it at
// protected-block entry does not.  Two levels on three edges, then three levels.
// ===========================================================================

// §28.10 (v0.252): two levels, NORMAL edge.  Establishment: FO(outer, at outer
// protected-block entry), a, FI(inner, at inner protected-block entry), b.
// Reverse: body marker 9, b=2, FI=5, a=1, FO=6 -> 92516.
// Pre-#2892 this was 95261 (each finally ran before its own body's local).
BEZEL_TEST(finally_established_at_try_entry_2892,
           nested_two_levels_normal_edge_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      var a := new A();
      try
        var b := new B();
        C._seq := C._seq * 10 + 9;
      finally
        C._seq := C._seq * 10 + 5;
      end;
    finally
      C._seq := C._seq * 10 + 6;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), "92516");
}

// §28.10 (v0.252): two levels, EXCEPTION edge through both.
// b=2, FI=5, a=1, FO=6, catch=7 -> 25167.  Pre-#2892 this was 52617.
BEZEL_TEST(finally_established_at_try_entry_2892,
           nested_two_levels_exception_edge_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var a := new A();
        try
          var b := new B();
          throw new E();
        finally
          C._seq := C._seq * 10 + 5;
        end;
      finally
        C._seq := C._seq * 10 + 6;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), "25167");
}

// §28.10 (v0.252): two levels, RETURN edge — 2516, UNCHANGED by #2892, and now
// the same interleave as the normal and exception edges above.  This is the
// #1598 `outerFr`-propagation shape (the mode-1 chain) with the resources moved
// INSIDE the protected blocks.
BEZEL_TEST(finally_established_at_try_entry_2892,
           nested_two_levels_return_edge_unchanged_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Inner(): int
    try
      var a := new A();
      try
        var b := new B();
        return 0;
      finally
        C._seq := C._seq * 10 + 5;
      end;
    finally
      C._seq := C._seq * 10 + 6;
    end;
  end Inner;
  public static method Main(): int
    C._seq := 0;
    _ := Inner();
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), "2516");
}

// §28.10 (v0.252): THREE levels on the exception edge — the strictest nesting
// check.  Establishment: F1, a, F2, b, F3, c.  Reverse: c=3, F3=9, b=2, F2=8,
// a=1, F1=7, catch=6 -> 3928176.  Pre-#2892 this was 9382716: every finally ran
// one step too early, which is exactly the nesting inversion the ruling names.
BEZEL_TEST(finally_established_at_try_entry_2892,
           nested_three_levels_exception_edge_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var a := new A();
        try
          var b := new B();
          try
            var c := new Cc();
            throw new E();
          finally
            C._seq := C._seq * 10 + 9;
          end;
        finally
          C._seq := C._seq * 10 + 8;
        end;
      finally
        C._seq := C._seq * 10 + 7;
      end;
    catch E => C._seq := C._seq * 10 + 6;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
class Cc destructor C._seq := C._seq * 10 + 3; end destructor; end Cc;
)"), "3928176");
}

// ===========================================================================
// A `throw` FROM THE `finally` — the edge on which "the body local is still in
// scope inside the finally" was the whole point of the retired reading.
// ===========================================================================

// §31.2 (v0.252): the protected block completes NORMALLY and the finally throws.
// The body local is already disposed when the finally starts, so it cannot be
// disposed a second time by the finally's own throw site: r1=1, finally=4,
// catch=7 -> 147.  Pre-#2892 this was 417.
BEZEL_TEST(finally_established_at_try_entry_2892,
           throw_from_finally_on_normal_edge_spec_31_2) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        _ := r1.V();
      finally
        C._seq := C._seq * 10 + 4;
        throw new E();
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    return C._seq;
  end Main;
end C;
class R1
  public method V(): int return 0; end V;
  destructor C._seq := C._seq * 10 + 1; end destructor;
end R1;
)"), 147);
}

// §31.2 (v0.252): "when a `finally` entered during exception propagation itself
// completes by throwing ... the new exception replaces the one being handled".
// The body local was disposed on the way out of the protected block, before the
// finally and before the supersession: r1=1, finally=4, catch F=7 -> 147.
// Pre-#2892 this was 417.
BEZEL_TEST(finally_established_at_try_entry_2892,
           throw_from_finally_supersedes_on_exception_edge_spec_31_2) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class E: Exception end E;
class F: Exception end F;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        throw new E();
      finally
        C._seq := C._seq * 10 + 4;
        throw new F();
      end;
    catch F => C._seq := C._seq * 10 + 7;
    end;
    return C._seq;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
)"), 147);
}

// ===========================================================================
// ASYNC — with an `await` inside the protected block the cleanup stack is the
// async frame's; the ranking must be the same across the suspend.
// ===========================================================================

// §30.9 / §28.10 (v0.252): async method, `await` inside the protected block,
// body-declared `using var`s after it, NORMAL fall-through.
// body marker 9, r2=2, r1=1, finally=4 -> 9214.  Pre-#2892 this was 9421.
BEZEL_TEST(finally_established_at_try_entry_2892,
           async_normal_edge_across_a_suspend_spec_30_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static async method Delay(): Task<int> return 9; end Delay;
  public static async method Run(): Task<int>
    try
      var w: int := await C.Delay();
      using var r1 := new R1();
      using var r2 := new R2();
      C._seq := C._seq * 10 + 9;
    finally
      C._seq := C._seq * 10 + 4;
    end;
    return 0;
  end Run;
  public static async method Main(): Task<int>
    C._seq := 0;
    var r: int := await C.Run();
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "9214");
}

// §30.9 / §28.10 (v0.252): the async EXCEPTION edge across a suspend.
// r2=2, r1=1, finally=4, catch=7 -> 2147.  Pre-#2892 this was 4217.
BEZEL_TEST(finally_established_at_try_entry_2892,
           async_exception_edge_across_a_suspend_spec_30_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static async method Delay(): Task<int> return 9; end Delay;
  public static async method Run(): Task<int>
    try
      try
        var w: int := await C.Delay();
        var r1 := new R1();
        var r2 := new R2();
        throw new E();
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    return 0;
  end Run;
  public static async method Main(): Task<int>
    C._seq := 0;
    var r: int := await C.Run();
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), "2147");
}

// §30.9 / §28.10 (v0.252): the async-method `return` edge (exit mode 5) across a
// suspend — 214, UNCHANGED by #2892.
BEZEL_TEST(finally_established_at_try_entry_2892,
           async_return_edge_across_a_suspend_unchanged_spec_30_9) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class C
  public static _seq: int := 0;
  public static async method Delay(): Task<int> return 9; end Delay;
  public static async method Run(): Task<int>
    try
      var w: int := await C.Delay();
      using var r1 := new R1();
      using var r2 := new R2();
      return 0;
    finally
      C._seq := C._seq * 10 + 4;
    end;
  end Run;
  public static async method Main(): Task<int>
    C._seq := 0;
    var r: int := await C.Run();
    return C._seq;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2;
)"), 214);
}

// ===========================================================================
// THE ITERATOR EDGES (§29.7) — already ranked the finally at protected-block
// entry before #2892 (the early-Dispose cleanup builder computes the same
// establishment order this fix gives the direct edges).  Guards that they stay.
// ===========================================================================

// §29.7 (v0.252): "[an iterator Dispose] runs the pending cleanup actions active
// at the point of suspension ... in reverse order of establishment, exactly as
// if the body had exited at the suspension point."  The consumer breaks while
// suspended inside the protected block: b=2, finally=4 -> 24.  UNCHANGED.
BEZEL_TEST(finally_established_at_try_entry_2892,
           iterator_early_dispose_edge_unchanged_spec_29_7) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Gen(): IEnumerable<int>
    try
      using var b := new B();
      yield return 1;
      yield return 2;
    finally
      C._seq := C._seq * 10 + 4;
    end;
  end Gen;
  public static method Main(): int
    C._seq := 0;
    for var x in Gen() do
      if x = 1 then break; end;
    end;
    return C._seq;
  end Main;
end C;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), 24);
}

// §29.7 (v0.252): the `yield break` edge — b=2, finally=4 -> 24.  UNCHANGED.
BEZEL_TEST(finally_established_at_try_entry_2892,
           yield_break_edge_unchanged_spec_29_7) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Gen(): IEnumerable<int>
    try
      using var b := new B();
      yield break;
    finally
      C._seq := C._seq * 10 + 4;
    end;
  end Gen;
  public static method Main(): int
    C._seq := 0;
    for var x in Gen() do
    end;
    return C._seq;
  end Main;
end C;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
)"), 24);
}

// §21.5 / §31.2 (v0.252): a labeled `break` crossing a finally to an outer loop,
// with a body-declared `using var c` inside the protected block: c=3,
// finally=9, b=2, a=1 -> 3921.  UNCHANGED by #2892 — this is the #2051 pin,
// re-stated here so the labeled edge is covered by this file's oracle too.
BEZEL_TEST(finally_established_at_try_entry_2892,
           labeled_break_across_finally_unchanged_spec_21_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    scan: for var i in 0..0 do
      using var a := new A();
      for var j in 0..0 do
        using var b := new B();
        try
          using var c := new Cc();
          break scan;
        finally
          C._seq := C._seq * 10 + 9;
        end;
      end;
    end scan;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class A destructor C._seq := C._seq * 10 + 1; end destructor; end A;
class B destructor C._seq := C._seq * 10 + 2; end destructor; end B;
class Cc destructor C._seq := C._seq * 10 + 3; end destructor; end Cc;
)"), "3921");
}

// ===========================================================================
// ADVERSARIAL ARM EDGES — the three shapes most likely to expose a wrong
// re-derivation, because each leaves the try by a path that is neither a plain
// fall-through nor a plain throw.
// ===========================================================================

// §31.2.1 (v0.252): a `when` GUARD that throws.  "If the guard itself throws,
// that new exception supersedes the in-flight one ... and propagates to the
// enclosing handler" — so it LEAVES this try and the finally owes a run (#2881).
// The body local was disposed by the ORIGINAL throw, before the guard ever ran:
// r1=1, finally=4, outer catch=7 -> 147.  Pre-#2892 this was 417 (the body scope
// was disposed by finUnwindBB's range, after the finally).
BEZEL_TEST(finally_established_at_try_entry_2892,
           guard_throw_edge_spec_31_2_1) {
    BEZEL_EXPECT_EQ(checked(R"(
module M;
class E: Exception end E;
class F: Exception end F;
class T
  public static method Boom(): bool
    throw new F();
  end Boom;
end T;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        throw new E();
      catch var e: E when T.Boom() => C._seq := C._seq * 10 + 8;
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch F => C._seq := C._seq * 10 + 7;
    end;
    return C._seq;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
)"), 147);
}

// §31.2.4 (v0.252): a matching arm that RE-RAISES (`throw;`).  The caught
// exception stays in flight for the enclosing handler; this try's finally still
// runs on the way out.  r1=1 (at the original throw), arm=8, finally=4, outer
// catch=7 -> 1847.  Pre-#2892 this was 8417.
BEZEL_TEST(finally_established_at_try_entry_2892,
           arm_reraise_edge_spec_31_2_4) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class E: Exception end E;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      try
        var r1 := new R1();
        throw new E();
      catch var e: E => do
        C._seq := C._seq * 10 + 8;
        throw;
      end
      finally
        C._seq := C._seq * 10 + 4;
      end;
    catch E => C._seq := C._seq * 10 + 7;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
)"), "1847");
}

// §21.5 (v0.252) / #1083: a `break` whose target loop was opened INSIDE the
// protected block does NOT exit the try, so it must not route through the
// finally — the try still completes normally afterwards.  body marker 9, r1=1,
// finally=4 -> 914.  Pre-#2892 this was 941.
BEZEL_TEST(finally_established_at_try_entry_2892,
           break_to_a_loop_inside_the_protected_block_spec_21_5) {
    BEZEL_EXPECT_EQ(checkedOut(R"(
module M;
class C
  public static _seq: int := 0;
  public static method Main(): int
    C._seq := 0;
    try
      var r1 := new R1();
      for var i in 0..3 do
        break;
      end;
      C._seq := C._seq * 10 + 9;
    finally
      C._seq := C._seq * 10 + 4;
    end;
    Console.WriteLine(C._seq);
    return 0;
  end Main;
end C;
class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1;
)"), "914");
}

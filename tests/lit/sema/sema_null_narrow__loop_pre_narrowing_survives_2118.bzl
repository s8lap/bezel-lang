// Spec §7.4 "How narrowing is invalidated" (v0.220, issue #2118): the loop-body
// narrowing join (which stops an in-body narrowing from leaking past the loop —
// see the reject track sema_null_narrow__loop_body_narrowing_does_not_leak_2118)
// must NOT drop a narrowing that was established BEFORE the loop and that the
// body never invalidates. Such a fact holds on BOTH the zero-iteration path
// (entry) and the >=1-iteration path (the body left it untouched), so it
// survives to the after-loop point. These programs must COMPILE with no
// diagnostic: after each loop `s` is still narrowed, so `s.Length` is legal.
//
// ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class C
  // `while`: `s` narrowed by the outer guard; the body only touches `total`/`i`,
  // never `s`, so the narrowing survives the loop and `s.Length` is accepted.
  public static method PreLoopSurvivesWhile(s: string?, n: int): int
    var total: int := 0;
    if s <> null then
      var i: int := 0;
      while i < n do
        total := total + 1;
        i := i + 1;
      end;
      return s.Length + total;
    end;
    return -1;
  end PreLoopSurvivesWhile;

  // `for`: same, over a range — the non-invalidating body preserves the
  // pre-loop narrowing of `s`.
  public static method PreLoopSurvivesFor(s: string?): int
    var total: int := 0;
    if s <> null then
      for var i in 0..3 do
        total := total + 1;
      end;
      return s.Length + total;
    end;
    return -1;
  end PreLoopSurvivesFor;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

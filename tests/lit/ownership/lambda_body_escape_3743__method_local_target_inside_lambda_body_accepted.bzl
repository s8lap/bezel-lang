// §28.3 / §28.9 (v0.286), issue #3743 — the ACCEPT boundary of the frame-chain
// widening in the OTHER direction: the SOURCE is in the enclosing frame and the
// TARGET is declared INSIDE the checked lambda body.
//
// The target dies when the lambda's activation ends; the source outlives that,
// so §28.3's "strictly encloses" premise is false the other way round and the
// store is not an escape. Whether the LAMBDA VALUE itself escapes the method is
// an analysis this pre-pass does not have (issue #3702's open half, recorded by
// fix #3564) — accepted before this fix and still accepted, deliberately.
//
// RUN: %bezelc -c %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class C
  public static method Run(): int
    using var r := new R();
    var mk := lambda(): int => do
      var f := lambda(): int => 0;
      f := lambda(): int => r.V();
      return f();
    end;
    return mk();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

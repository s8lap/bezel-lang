//! case: s28_9_8524
//! doc: spec
//! section: 28.9
//! title: a capturing closure kept in two Object fields stays valid in the second after the first
//!   holder dies
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 1594
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. One closure over `j = k * 3` is kept in two holders; holder `a`
//!   runs it and dies at the end of its `do` block, then holder `b` runs it. Each iteration
//!   adds 2 * (3k + 1) with k = i mod 3: 2 + 8 + 14 = 24 per cycle, 66 cycles = 1584, and i
//!   = 198, 199 add 2 and 8: 1594.
module Conf;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Get(): Object? => _o;
  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
end Hx;

public class H
  public static method Fill2(a: Hx, b: Hx, k: int)
    var j := k * 3;
    var f: () -> int := lambda() => j + 1;
    a.Keep(f);
    b.Keep(f);
  end Fill2;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var b := new Hx();
      do
        var a := new Hx();
        H.Fill2(a, b, i mod 3);
        t := t + a.Run();
      end do;
      t := t + b.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;

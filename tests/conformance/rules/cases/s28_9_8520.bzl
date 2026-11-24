//! case: s28_9_8520
//! doc: spec
//! section: 28.9
//! title: a function value that overwrites its own field while it runs completes normally
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure
//!   object that backs it
//! cites: 28.9, 8.1
//! expect: run
//! stdout: 399
//! notes: Pins #9061. A function value stored into an `Object` field is runtime-managed (§28.9):
//!   displacing it from the field, or the field's holder dying, must leave every other
//!   reference to it valid. The closure kept in the field overwrites that field with a
//!   second closure through `x.Keep`, and then returns k + 1 with k = i mod 3. Per three
//!   iterations 1 + 2 + 3 = 6; 66 full cycles give 396, and i = 198, 199 add 1 and 2: 399.
module Conf;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Run(): int => (_o as (Hx) -> int)!(this);
end Hx;
public class H
  public static method Fill(r: Hx, k: int)
    var f: (Hx) -> int := lambda(x: Hx) => do x.Keep(lambda() => 0); return k + 1; end;
    r.Keep(f);
  end Fill;
end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx();
      H.Fill(h, i mod 3);
      t := t + h.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;

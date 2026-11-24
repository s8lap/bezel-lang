//! case: s28_9_8605
//! doc: spec
//! section: 28.9
//! title: a call through a field typed over the type parameter survives the field being cleared
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b6b_generic_t). The field type (Gx<T>, T) -> int mentions T; Run passes this and the stored value. The closure clears the field and returns k+1. Sum = 399.
module Conf;
import System;
public class Gx<T>
  private _f: ((Gx<T>, T) -> int)? := null;
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Fill(k: int) _f := lambda(x: Gx<T>, v: T) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public method Run(): int
    if _f <> null then return _f!(this, _v); end if;
    return 7;
  end Run;
end Gx;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Gx<int>(3); h.Fill(i mod 3); t := t + h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;

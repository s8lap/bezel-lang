//! case: s28_9_7447
//! doc: spec
//! section: 28.9
//! title: a string returned from either return of an immediately-invoked lambda keeps its value
//! rule: `string` — pervasive; created by literals, interpolation, concatenation, and library
//!   methods.
//! cites: 28.9, 27.5
//! expect: run
//! output: 990
module Conf;
public class Box
  public V: int := 0;
  public constructor(v: int)
    V := v;
  end constructor;
end Box;
public class H
  public static readonly Cfg: string := "cfgstring";
  private _s: string;
  private _f: string := "init";
  public constructor(s: string)
    _s := s;
  end constructor;

end H;
public class C
  public static method Take(s: string): int return s.Length; end Take;
  public static method Run(t: string, c: bool): int
    var r := (lambda(b: bool) => do if b then return t + "q"; end; return t; end)(c); return r.Length;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("ab" + j.ToString(), j mod 2 = 0); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;

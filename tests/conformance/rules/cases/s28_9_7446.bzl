//! case: s28_9_7446
//! doc: spec
//! section: 28.9
//! title: a string built by an immediately-invoked lambda returned from a method keeps its value
//! rule: `string` — pervasive; created by literals, interpolation, concatenation, and library
//!   methods.
//! cites: 28.9, 27.5
//! expect: run
//! output: 1980
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
  public static method R1(t: string): string
    return (lambda() => t + "q")();
  end R1;
  public static method R2(t: string): string
    return (lambda(x: string) => x)(t);
  end R2;
end H;
public class C
  public static method Take(s: string): int return s.Length; end Take;
  public static method Run(t: string, c: bool): int
    var a := H.R1(t); var b := H.R2(t); return a.Length + b.Length;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("ab" + j.ToString(), j mod 2 = 0); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;

//! case: s28_9_7448
//! doc: spec
//! section: 28.9
//! title: a nullable string produced by an immediately-invoked lambda keeps its value
//! rule: `string` — pervasive; created by literals, interpolation, concatenation, and library
//!   methods.
//! cites: 28.9, 27.5
//! expect: run
//! output: 545
module Conf;
public class C
  public static method Run(t: string, c: bool): int
    var n: string? := (lambda(): string? => (if c then t + "q" else null))(); if n <> null then return n.Length; end; return 0;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("ab" + j.ToString(), j mod 2 = 0); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;

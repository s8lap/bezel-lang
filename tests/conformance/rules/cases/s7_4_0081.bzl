//! case: s7_4_0081
//! doc: spec
//! section: 7.4
//! title: a while condition re-establishes a readonly field narrowing before each iteration reads it
//! rule: "a `while` condition" establishes narrowing ([§7.4]), and "in `A and B`, `B` (and the true branch) is checked under `A`'s true-facts" ([§7.4]). The reassigned root is re-tested by the condition before the next body read.
//! expect: run
//! output: 1
//!   1
module Conf;
public record Pt(X: int, Y: int);
public class H
  public readonly F: Pt?;
  public constructor(f: Pt?) this.F := f; end constructor;
end H;
public class P
  public static method Main(): int
    var h := new H(new Pt(1, 2)); var i := 0;
    while i < 3 and h.F <> null do Console.WriteLine(h.F.X); h := new H(null); i += 1; end;
    Console.WriteLine(i);
    return 0;
  end Main;
end P;

//! case: s7_4_0077
//! doc: spec
//! section: 7.4
//! title: a root reassignment followed by break leaves no readonly field fact on the loop back-edge
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]): the path that reassigns the root leaves the loop by break and never reaches the read. "(or `throw`, `break`, `continue` in that branch)" ([§7.4]) is an early exit, and "a fact rides its root (reassigning `v` drops it)" ([§7.4]).
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
    if h.F <> null then
      while i < 2 do
        Console.WriteLine(h.F.X); i += 1;
        if i = 2 then h := new H(null); break; end;
      end;
    end;
    return 0;
  end Main;
end P;

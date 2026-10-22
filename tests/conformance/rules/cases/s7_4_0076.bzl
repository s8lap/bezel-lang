//! case: s7_4_0076
//! doc: spec
//! section: 7.4
//! title: a test inside the loop body re-establishes a readonly field narrowing before the read
//! rule: "`x <> null` or `x is not null` (narrowing the true branch)" ([§7.4]). "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]): the body test is on every path to the read, so the reassigned root does not reach it unnarrowed.
//! expect: run
//! output: 1
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
        if h.F <> null then Console.WriteLine(h.F.X); end; h := new H(null); i += 1;
      end;
    end;
    return 0;
  end Main;
end P;

//! case: s7_4_0084
//! doc: spec
//! section: 7.4
//! title: a readonly field narrowing is dropped by a root reassignment followed by continue inside a try
//! rule: "a fact rides its root (reassigning `v` drops it)" ([§7.4]). The narrowing of a readonly field is dropped when its root local is reassigned, whatever the new value's type.
//!   "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]): the loop back-edge carries the reassigned root to the read at the top of the next iteration.
//! expect: reject
//! diagnostic: the knowledge that this `readonly` field is non-null was established before the loop
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
        i += 1; Console.WriteLine(h.F.X); try if i = 1 then h := new H(null); continue; end; finally Console.WriteLine(9); end;
      end;
    end;
    return 0;
  end Main;
end P;

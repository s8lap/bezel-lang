//! case: s7_4_0078
//! doc: spec
//! section: 7.4
//! title: a readonly field re-tested after its root is reassigned is narrowed for the read
//! rule: "`x <> null` or `x is not null` (narrowing the true branch)" ([§7.4]). "a fact rides its root (reassigning `v` drops it)" ([§7.4]): the reassignment drops the old fact, and the test that follows the reassignment establishes a new one.
//! expect: run
//! output: 1
//!   5
//!   5
module Conf;
public record Pt(X: int, Y: int);
public class H
  public readonly F: Pt?;
  public constructor(f: Pt?) this.F := f; end constructor;
end H;
public class P
  public static method Main(): int
    var h := new H(new Pt(1, 2)); var i := 0;
    while i < 3 do
      if h.F = null then h := new H(new Pt(5, 5)); end;
      if h.F <> null then Console.WriteLine(h.F.X); end;
      h := new H(null); i += 1;
    end;
    return 0;
  end Main;
end P;

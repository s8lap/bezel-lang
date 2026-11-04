//! case: s7_4_0180
//! doc: spec
//! section: 7.4
//! title: reassigning the root parameter drops the fact about its readonly function-typed field
//! rule: a fact rides its root (reassigning `v` drops it)
//! cites: 12.7
//! expect: reject
//! diagnostic: narrow
//! issue: 8099
module Conf;

public class H
  public readonly F: ((int) -> int)?;

  public constructor()
    F := lambda(x: int) => x + 1;
  end constructor;
end H;

public class Program
  public static method Run(h: H, w: H): int
    if h.F is (int) -> int then
      h := w;
      return h.F(1);
    end if;
    return 0;
  end Run;

  public static method Main()
    using var h := new H();
    using var w := new H();
    Console.WriteLine(Run(h, w));
  end Main;
end Program;

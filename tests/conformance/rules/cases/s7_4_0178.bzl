//! case: s7_4_0178
//! doc: spec
//! section: 7.4
//! title: a fact on a readonly field of a parameter root does not reach a lambda formed under the guard
//! rule: so a test on a `var` local or a parameter does not reach a lambda formed under it
//! cites: 27.5
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
  public static method Run(h: H): int
    if h.F is (int) -> int then
      var g := lambda(): int => h.F(1);
      return g();
    end if;
    return 0;
  end Run;

  public static method Main()
    using var h := new H();
    Console.WriteLine(Run(h));
  end Main;
end Program;

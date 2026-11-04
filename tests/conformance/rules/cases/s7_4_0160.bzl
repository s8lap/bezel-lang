//! case: s7_4_0160
//! doc: spec
//! section: 7.4
//! title: a null test on a nullable function parameter does not reach a lambda formed under it that
//!   invokes it
//! rule: so a test on a `var` local or a parameter does not reach a lambda formed under it
//! cites: 27.5
//! expect: reject
//! diagnostic: narrow
//! issue: 8099
module Conf;

public class Program
  public static method Run(h: ((int) -> int)?): int
    if h <> null then
      var g := lambda(): int => h(1);
      return g();
    end if;
    return 0;
  end Run;

  public static method Main()
    Console.WriteLine(Run(lambda(x: int) => x + 1));
  end Main;
end Program;

//! case: s7_4_0157
//! doc: spec
//! section: 7.4
//! title: a nullable function parameter narrowed by a successful `is` test can be invoked in the block
//! rule: inside the controlled block of a successful `is` test
//! cites: 7.6, 12.7
//! expect: run
//! output: 2
//!   0
module Conf;

public class Program
  public static method Run(h: ((int) -> int)?): int
    if h is (int) -> int then
      return h(1);
    end if;
    return 0;
  end Run;

  public static method Main()
    Console.WriteLine(Run(lambda(x: int) => x + 1));
    Console.WriteLine(Run(null));
  end Main;
end Program;

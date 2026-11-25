//! case: s8_5_9401
//! doc: spec
//! section: 8.5
//! title: a bare method name held at Object renders as its formed function type
//! rule: a lambda and a method reference render alike
//! cites: 8.5, 27.6
//! expect: run
//! output: (int) -> int
module Conf;

public class Program
  static method P(i: int): int => i + 100;
  public static method Main(): int
    var o: Object := P;
    Console.WriteLine(o);
    return 0;
  end Main;
end Program;

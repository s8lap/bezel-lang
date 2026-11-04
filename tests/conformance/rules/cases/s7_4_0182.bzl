//! case: s7_4_0182
//! doc: spec
//! section: 7.4
//! title: copying a ref parameter of nullable function type to a local and narrowing the copy admits the
//!   call
//! rule: The standard pattern is the field pattern — copy to a local and narrow the copy:
//! cites: 22.3
//! expect: run
//! output: 2
//! issue: 8099
module Conf;

public class Program
  public static method Run(ref p: ((int) -> int)?): int
    var c := p;
    if c is (int) -> int then
      return c(1);
    end if;
    return 0;
  end Run;

  public static method Main()
    var h: ((int) -> int)? := lambda(x: int) => x + 1;
    Console.WriteLine(Run(ref h));
  end Main;
end Program;

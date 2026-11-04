//! case: s7_4_0181
//! doc: spec
//! section: 7.4
//! title: a type test on a ref parameter of nullable function type narrows nothing
//! rule: A null test, type test ([§12.7](#sec-type-tests-is-is-not-as)), assignment, `??=`, early exit,
//!   or `out`-recipient postcondition ([§22.4](#sec-out-nullability-postcondition)) on it narrows
//!   nothing
//! cites: 22.3, 27.1
//! expect: reject
//! diagnostic: narrow
//! issue: 8099
module Conf;

public class Program
  public static method Run(ref p: ((int) -> int)?): int
    if p is (int) -> int then
      return p(1);
    end if;
    return 0;
  end Run;

  public static method Main()
    var h: ((int) -> int)? := lambda(x: int) => x + 1;
    Console.WriteLine(Run(ref h));
  end Main;
end Program;

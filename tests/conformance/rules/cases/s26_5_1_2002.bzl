//! case: s26_5_1_2002
//! doc: spec
//! section: 26.5.1
//! title: a user Object extension Where with the stdlib arity loses to the
//!   more specific IEnumerable Where
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is
//!   a subtype of E2 and E2 is not a subtype of E1.
//! cites: 26.5, 5
//! expect: run
//! stdout: 2
module Conf;

extension Object
  public method Where(f: (int) -> bool): int => 4242;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Where(lambda(x) => x > 1).Count());
  end Main;
end Program;

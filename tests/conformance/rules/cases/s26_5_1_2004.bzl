//! case: s26_5_1_2004
//! doc: spec
//! section: 26.5.1
//! title: a user Object extension Select overloaded by arity does not
//!   displace the stdlib one-selector Select
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5, 5
//! expect: run
//! output: 1
//!   7
//!   30
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Select(): int => 1;
  public method Select(x: int, y: int): int => x + y;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Select());
    Console.WriteLine(a.Where(lambda(x) => x > 0).Select(3, 4));
    Console.WriteLine(a.Where(lambda(x) => x > 0).Select(lambda(x) => x * 10).Sum());
  end Main;
end Program;

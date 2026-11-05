//! case: s26_5_1_2005
//! doc: spec
//! section: 26.5.1
//! title: a user Object extension Skip(a, b) is reached after a lazy query;
//!   the stdlib Skip(n) still applies
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5, 5
//! expect: run
//! output: 42
//!   1
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Skip(a: int, b: int): int => a * b;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
    Console.WriteLine(a.Where(lambda(x) => x > 1).Select(lambda(x) => x * 2).Skip(6, 7));
    Console.WriteLine(a.Where(lambda(x) => x > 1).Skip(1).Count());
  end Main;
end Program;

//! case: s26_5_1_2008
//! doc: spec
//! section: 26.5.1
//! title: an Object extension with the right arity and matching argument
//!   types is applicable on a lazy query
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5
//! expect: run
//! stdout: 7
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Take(x: int, y: int): int => 7;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Take(1, 2));
  end Main;
end Program;

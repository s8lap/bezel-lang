//! case: s26_5_1_2007
//! doc: spec
//! section: 26.5.1
//! title: an Object extension of the right arity but mismatched argument
//!   types leaves no candidate
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5
//! expect: reject
//! diagnostic: Take
module Conf;

extension Object
  public method Take(x: string, y: string): int => 7;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Take(1, "s"));
  end Main;
end Program;

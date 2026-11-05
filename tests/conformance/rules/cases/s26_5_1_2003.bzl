//! case: s26_5_1_2003
//! doc: spec
//! section: 26.5.1
//! title: user Object extensions Select() and Take(x, y) at other arities
//!   are reached on a lazy query
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5
//! expect: run
//! output: ext-str
//!   1.5
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Select(): string => "ext-str";
  public method Take(x: int, y: int): double => 1.5;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2);
    var s: string := a.Where(lambda(x) => x > 0).Select();
    Console.WriteLine(s);
    var d: double := a.Where(lambda(x) => x > 0).Take(3, 4);
    Console.WriteLine(d);
  end Main;
end Program;

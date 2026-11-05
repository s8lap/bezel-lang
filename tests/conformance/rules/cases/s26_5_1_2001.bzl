//! case: s26_5_1_2001
//! doc: spec
//! section: 26.5.1
//! title: a zero-argument user Object extension named Select is reached on
//!   a lazy query receiver
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5, 29.11
//! expect: run
//! stdout: 424200
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Select(): int => 4242;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2);
    var t := 0;
    for var i in 0..<100 do t := t + a.Where(lambda(x) => x > 0).Select(); end;
    Console.WriteLine(t);
  end Main;
end Program;

//! case: s26_5_1_2006
//! doc: spec
//! section: 26.5.1
//! title: a zero-argument user Object extension Select using this is
//!   reached on a bound lazy query
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5, 29.11
//! expect: run
//! stdout: True
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module Conf;

extension Object
  public method Select(): string => this.ToString();
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    var q := a.Where(lambda(x) => x > 0);
    Console.WriteLine(q.Select().Length >= 0);
  end Main;
end Program;

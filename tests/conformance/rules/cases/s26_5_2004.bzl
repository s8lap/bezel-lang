//! case: s26_5_2004
//! doc: spec
//! section: 26.5
//! title: a zero-argument Where on a lazy query is an error when no
//!   extension supplies that arity
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5.1
//! expect: reject
//! diagnostic: Where
module Conf;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Where());
  end Main;
end Program;

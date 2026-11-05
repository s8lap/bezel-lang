//! case: s26_5_2003
//! doc: spec
//! section: 26.5
//! title: a zero-argument Select on a Dictionary receiver is an error when
//!   no extension supplies it
//! rule: specificity ranks the applicable candidates: extension members of
//!   the called name that match the call's arity and argument types,
//!   from blocks whose head matches R
//! cites: 26.5.1
//! expect: reject
//! diagnostic: Select
module Conf;

public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d.Add("a", 1);
    Console.WriteLine(d.Select());
  end Main;
end Program;

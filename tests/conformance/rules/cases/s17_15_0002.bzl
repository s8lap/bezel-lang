//! case: s17_15_0002
//! doc: spec
//! section: 17.15
//! title: multiple clauses chain, each terminated by ;
//! rule: "`(var a := E1; var b := E2; body)` ... at least one clause is
//!   required; clauses chain."
//! cites: 17.15
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var r := (var a := 2; var b := 3; a + b);
    Console.WriteLine(r);
  end Main;
end Program;

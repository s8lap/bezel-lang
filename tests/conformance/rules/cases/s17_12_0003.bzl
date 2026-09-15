//! case: s17_12_0003
//! doc: spec
//! section: 17.12
//! title: bare throw outside a catch arm body is a compile error
//! rule: "Bare `throw` outside a catch arm body is a compile error, with the
//!   same diagnostic as the statement form `throw;` outside a catch
//!   context."
//! cites: 17.12, 31.2.4
//! expect: reject
//! diagnostic: catch
module Conf;

public class Program
  public static method Main()
    var c := true;
    var v: int := (if c then 5 else throw);
    Console.WriteLine(v);
  end Main;
end Program;

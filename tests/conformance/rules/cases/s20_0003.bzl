//! case: s20_0003
//! doc: spec
//! section: 20
//! title: the else branch of an if expression is mandatory
//! rule: "the `else` branch is mandatory (an expression produces a value on every
//!   path)."
//! cites: 20
//! expect: reject
//! diagnostic: else
module Conf;

public class Program
  public static method Main()
    var x := (if true then 1);
  end Main;
end Program;

//! case: s21_6_0001
//! doc: spec
//! section: 21.6
//! title: a local declared inside a do end block is out of scope after it ends
//! rule: Locals declared in a block are visible only until the matching
//!   `end`.
//! cites: 21.6
//! expect: reject
//! diagnostic: scope
module Conf;

public class Program
  public static method Main()
    do
      var x := 5;
    end;
    Console.WriteLine(x.ToString());
  end Main;
end Program;

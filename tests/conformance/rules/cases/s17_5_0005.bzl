//! case: s17_5_0005
//! doc: spec
//! section: 17.5
//! title: negated membership is written not (x in c)
//! rule: "Negated membership is written `not (x in c)` — the parentheses are
//!   required because unary `not` binds tighter than the relational `in`."
//! cites: 17.5, 17.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var numbers: Array<int> := [10, 20, 42];
    var absent := not (99 in numbers);
    Console.WriteLine(absent);
  end Main;
end Program;

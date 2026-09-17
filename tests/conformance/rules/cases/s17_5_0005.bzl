//! case: s17_5_0005
//! doc: spec
//! section: 17.5
//! title: not x in c parses as (not x) in c and is rejected for a non-bool x
//! rule: "Negated membership is written `not (x in c)` — the parentheses are
//!   required because unary `not` binds tighter than the relational `in`
//!   (§17.1), so `not x in c` parses as `(not x) in c`, a type error for a
//!   non-`bool` `x`."
//! cites: 17.5, 17.1
//! expect: reject
//! diagnostic: bool
module Conf;

public class Program
  public static method Main()
    var numbers: Array<int> := [10, 20, 42];
    var absent := not 99 in numbers;
    Console.WriteLine(absent);
  end Main;
end Program;

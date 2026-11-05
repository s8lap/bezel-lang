//! case: s25_2_1206
//! doc: spec
//! section: 25.2
//! title:int plus a DateTime has no applicable operator
//! rule:applies is a compile error at the expression
//! cites: 25.1, 2.5
//! expect: reject
//! diagnostic: operator
module Conf;

public class Program
  public static method Main()
    var a := new DateTime(2020, 1, 1);
    var r := 1 + a;
    Console.WriteLine(r);
  end Main;
end Program;

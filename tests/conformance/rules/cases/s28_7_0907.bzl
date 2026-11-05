//! case: s28_7_0907
//! doc: spec
//! section: 28.7
//! title: reassigning a nullable owned Array local while a postfix-bang alias is read afterwards is
//!   rejected
//! rule: Reassigning a plain-`var` owned **local** while a live local borrow of its current value exists
//!   is rejected by the consume-site freeze
//! cites: 28.7, 28.3.9
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static method Main()
    var t: string := "a,bc,dc";
    var k := 0;
    var s: string? := "a,bc,dc";
    var a := s?.Split(",");
    var b := a!;
    a := s?.Split(";");
    k := k + b.Count;

    Console.WriteLine(k);
  end Main;
end Program;

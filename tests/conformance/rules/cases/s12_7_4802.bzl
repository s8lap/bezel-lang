//! case: s12_7_4802
//! doc: spec
//! section: 12.7
//! title: is and is not on a nullable value-type target answer as the underlying type over foreign boxes
//! rule: tests as its underlying type: `x is T?` has the answer of `x is T`
//! cites: 12.7, 17.4
//! expect: run
//! output: False
//!   True
//!   True
//!   False
//!   False
//!   False
//!   True
//!   False
//!   True
//!   False
//! issue: 8593
module Conf;

public class Program
  public static method Main()
    var o: Object := new DateTime(2020, 1, 1);
    Console.WriteLine(o is Guid?);
    Console.WriteLine(o is not Guid?);
    Console.WriteLine(o is DateTime?);
    Console.WriteLine(o is not DateTime?);
    var g: Object := Guid.Empty;
    Console.WriteLine(g is DateTime?);
    Console.WriteLine(g is TimeSpan?);
    Console.WriteLine(g is Guid?);
    var s: Object := "str";
    Console.WriteLine(s is Guid?);
    var x: Guid? := s as Guid?;
    Console.WriteLine(x = null);
    var y: DateTime? := o as DateTime?;
    Console.WriteLine(y = null);
  end Main;
end Program;

//! case: s12_7_4804
//! doc: spec
//! section: 12.7
//! title: is and as with a nullable record and a nullable enum target match their underlying type
//! rule: tests as its underlying type: `x is T?` has the answer of `x is T`
//! cites: 12.7, 17.4
//! expect: run
//! output: True
//!   1
//!   True
//!   True
//!   False
//! issue: 8593
module Conf;

public record Pt(X: int, Y: int);

public enum Color
  Red,
  Green;
end Color;

public class Program
  public static method Main()
    var o: Object := new Pt(1, 2);
    Console.WriteLine(o is Pt?);
    var p: Pt? := o as Pt?;
    Console.WriteLine(p!.X);
    var e: Object := Color.Green;
    Console.WriteLine(e is Color?);
    var c: Color? := e as Color?;
    Console.WriteLine(c! = Color.Green);
    Console.WriteLine(o is Color?);
  end Main;
end Program;

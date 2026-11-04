//! case: s8_5_0020
//! doc: spec
//! section: 8.5
//! title: an inline array of a user record renders its FullName, the element qualified
//! rule: An inline array's name is its stdlib §2.13 `FullName` rendering (rule
//!   4, the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in module
//!   `M`.
//! cites: 8.5, 15.1
//! expect: run
//! stdout: [2]Conf.Rec
//! issue: 7794
module Conf;

public record Rec(X: int);

public class Program
  public static method Main()
    var a: [2]Rec := [new Rec(1), new Rec(2)];
    Console.WriteLine(a.ToString());
  end Main;
end Program;

//! case: s8_5_2303
//! doc: spec
//! section: 8.5
//! title: an inline array of Guid renders its element as System.Guid
//! rule: the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in
//!   module `M`
//! cites: 2.3, 2.13
//! expect: run
//! output: [1]System.Guid
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[1]Guid`), the staging tip prints the FullName rendering. Guid is
//!   declared in module System (spec 2.3).
module Conf;

public class Program
  public static method Main()
    var g: [1]Guid := [Guid.Empty];
    Console.WriteLine(g.ToString());
  end Main;
end Program;

//! case: s8_5_2306
//! doc: spec
//! section: 8.5
//! title: an inline array of an imported record renders the DECLARING
//!   module, not the current one
//! rule: the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in
//!   module `M`
//! cites: 2.13, 2.8
//! expect: run
//! output: [2]Lib.Rec
//!   [2]Lib.Rec
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[2]Rec`), the staging tip prints the FullName rendering. Directory
//!   case: the element's declaring module Lib differs from the program's
//!   module App.
module App;

import Lib;

public class Program
  public static method Main()
    var a: [2]Rec := [new Rec(1), new Rec(2)];
    Console.WriteLine(a.ToString());
    Console.WriteLine($"{a}");
  end Main;
end Program;

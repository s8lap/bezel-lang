//! case: s8_5_2307
//! doc: spec
//! section: 8.5
//! title: an inline array of an imported enum renders the declaring module
//! rule: the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in
//!   module `M`
//! cites: 2.13, 10.5
//! expect: run
//! output: [1]Lib.Col
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[1]Col`), the staging tip prints the FullName rendering. Directory
//!   case.
module App;

import Lib;

public class Program
  public static method Main()
    var c: [1]Col := [Col.Red];
    Console.WriteLine(c.ToString());
  end Main;
end Program;

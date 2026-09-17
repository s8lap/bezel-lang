//! case: s26_5_0014
//! doc: spec
//! section: 26.5
//! title: a public extension block is visible from an importing module
//! rule: The compiler considers only extension members visible at the call
//!   site. Marking the block `public` makes its member a candidate for any
//!   importing module.
//! cites: 26.5, 26.3, 26.1
//! expect: run
//! stdout: 12
module App;
import Lib;

public class Program
  public static method Main()
    using var w := new Widget(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;

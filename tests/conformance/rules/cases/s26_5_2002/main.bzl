//! case: s26_5_2002
//! doc: spec
//! section: 26.5
//! title: a user Object extension declared in an imported module is reached
//!   on a lazy query receiver
//! rule: searches the current module and all imported modules
//! cites: 26.5.1
//! expect: run
//! stdout: 4242
//! notes: Failed on the compiler before the #8179 fix and passes on the
//!   staging tip.
module App;

import Lib;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1); a.Add(2);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Select());
  end Main;
end Program;

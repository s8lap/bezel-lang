//! case: s17_15_0007
//! doc: spec
//! section: 17.15
//! title: an owned production undischarged in the atom is disposed at the atom boundary
//! rule: "an owned production makes its binding a **named unbound owned
//!   temporary whose boundary is the atom itself**: if still undischarged
//!   when the atom completes, it is disposed there ... with no must-use
//!   warning."
//! cites: 17.15, 28.3
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var n := (var w := new Widget(); 1);
    Console.WriteLine(n);
  end Main;
end Program;

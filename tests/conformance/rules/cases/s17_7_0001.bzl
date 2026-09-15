//! case: s17_7_0001
//! doc: spec
//! section: 17.7
//! title: new on a class produces an owned reference, undischarged draws the must-use warning
//! rule: "`new T(args)` constructs an instance of T. For classes, `new`
//!   produces an owned reference (§28)." §28.3 K3.14: "the implicit drop of a
//!   plain-`var` local ... carries the must-use warning". The class declares a
//!   destructor so it is not trivial-dispose (§28.9), whose ceremony
//!   exemption would otherwise free it silently with no warning.
//! cites: 17.7, 28.3, 28.9
//! expect: warn
//! diagnostic: must use
module Conf;

public class Connection
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Connection;

public class Program
  public static method Main()
    var c := new Connection();
  end Main;
end Program;

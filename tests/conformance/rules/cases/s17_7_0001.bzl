//! case: s17_7_0001
//! doc: spec
//! section: 17.7
//! title: new on a class produces an owned reference, undischarged draws the must-use warning
//! rule: "`new T(args)` constructs an instance of T. For classes, `new`
//!   produces an owned reference (§28)."
//! cites: 17.7, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    var c := new Connection();
  end Main;
end Program;

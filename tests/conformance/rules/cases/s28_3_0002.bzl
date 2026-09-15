//! case: s28_3_0002
//! doc: spec
//! section: 28.3
//! title: an undischarged plain-var owned local raises the must-use warning
//! rule: The compiler emits a warning at the implicit-drop site of a plain-`var` owned
//!   local, to surface a drop that may have been unintended; the exception edge does
//!   not carry it.
//! cites: 28.3, C.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Resource;

public class Program
  public static method Main()
    var r := new Resource();
  end Main;
end Program;

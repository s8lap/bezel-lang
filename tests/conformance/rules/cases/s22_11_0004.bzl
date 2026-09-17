//! case: s22_11_0004
//! doc: spec
//! section: 22.11
//! title: binding the instance to a local and assigning members replaces the retired form
//! rule: In its place, ... bind the instance to a local and assign its
//!   members as ordinary statements, whose exception-path disposal is
//!   §28.3's general discharge of an owned local.
//! cites: 22.11, 28.3
//! expect: run
//! stdout: b
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget("a");
    w.Name := "b";
    Console.WriteLine(w.Name);
  end Main;
end Program;

//! case: s26_5_0001
//! doc: spec
//! section: 26.5
//! title: an instance member wins over a same-named extension member
//! rule: Instance members win — by name. If the receiver's declared type declares
//!   any instance member of that name, instance resolution is committed and
//!   extension blocks are never consulted for that name on that receiver.
//! cites: 26.5
//! expect: run
//! stdout: instance
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe(): string => "instance";
end Widget;

extension Widget
  public method Describe(): string => "extension";
end;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Describe());
  end Main;
end Program;

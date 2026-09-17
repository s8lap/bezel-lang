//! case: s26_5_0003
//! doc: spec
//! section: 26.5
//! title: an extension member is reached when the type declares no such name
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules for extension
//!   blocks on the receiver's type, base types, or implemented interfaces.
//! cites: 26.5
//! expect: run
//! stdout: 12
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  public method Doubled(): int => this.Size * 2;
end;

public class Program
  public static method Main()
    using var w := new Widget(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;

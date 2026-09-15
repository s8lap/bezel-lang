//! case: s22_11_0003
//! doc: spec
//! section: 22.11
//! title: named and optional constructor arguments replace the retired object-initializer block
//! rule: In its place, use named/optional constructor arguments — which
//!   run the constructor and respect its invariants.
//! cites: 22.11, 22.5
//! expect: run
//! stdout: 5
module Conf;

public class Widget
  public property Name: string;
  public property Size: int;

  public constructor(name: string, size: int := 1)
    Name := name;
    Size := size;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget(name: "gadget", size: 5);
    Console.WriteLine(w.Size);
  end Main;
end Program;

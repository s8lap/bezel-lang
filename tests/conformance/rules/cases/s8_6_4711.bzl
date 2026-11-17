//! case: s8_6_4711
//! doc: spec
//! section: 8.6
//! title: a narrowed Guid? at a non-generic Guid parameter returned as Object boxes the bare Guid
//! rule: The nullable wrapper is stripped during boxing.
//! cites: 8.6, 22.6
//! expect: run
//! stdout: True
//! issue: 8607
module Conf;

public class Program
  public static method F(g: Guid): Object
    return g;
  end F;

  public static method Main()
    var n: Guid? := Guid.Empty;
    var f := F(n);
    Console.WriteLine(f.Equals(Guid.Empty));
  end Main;
end Program;

//! case: s9_2_0014
//! doc: spec
//! section: 9.2
//! title: a zero-auto-property body record compares equal always and renders TypeName()
//! rule: A body-only record with zero auto-properties is auto-hashable; the
//!   generated = returns true for every pair of values, and the generated
//!   ToString produces "TypeName()".
//! expect: run
//! stdout: True,Nothing()
module Conf;

public record Nothing
  public constructor()
  end constructor;
end Nothing;

public class Program
  public static method Main()
    var a := new Nothing();
    var b := new Nothing();
    Console.WriteLine($"{a = b},{a.ToString()}");
  end Main;
end Program;

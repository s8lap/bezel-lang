//! case: s22_6_0008
//! doc: spec
//! section: 22.6
//! title: example 5 -- no reachable candidate cites no candidates
//! rule: "If no candidate is applicable at all ... the call is a compile
//!   error with no candidates cited, because no overload was reachable."
//! cites: 22.6
//! expect: reject
//! diagnostic: no candidate
module Conf;

public class Transformer
  public constructor()
  end constructor;

  public method Transform(x: int): string => "int";
  public method Transform(x: decimal): string => "decimal";
end Transformer;

public class Program
  public static method Main()
    var t := new Transformer();
    var n: float := 2.5f;
    Console.WriteLine(t.Transform(n));
  end Main;
end Program;

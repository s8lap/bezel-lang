//! case: s19_3_0027
//! doc: spec
//! section: 19.3
//! title: a choice case head whose name is also a record type in scope names the case over a choice scrutinee
//! rule: Resolution honours ordinary member visibility (section 13). A slot head written Sq over a choice scrutinee names the choice case Sq (section 35.5), so the record Sq's private field is not the slot's member and the case payload is read.
//! cites: 19.3, 35.5, 13.1
//! expect: run
//! stdout: 4
//! issue: 6045
module Conf;

public record Sq
  private Side: int;

  public constructor(x: int)
    Side := x;
  end constructor;
end Sq;

public choice Shape Sq(Side: int), Tri(Base: int); end Shape;

public class Program
  public static method Main()
    var s: Shape := Shape.Sq(4);
    case s of Sq(Side = var v) => Console.WriteLine(v.ToString());,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;

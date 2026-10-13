//! case: s19_3_0020
//! doc: spec
//! section: 19.3
//! title: a private field of a record may not be named in a slot through a type alias head
//! rule: Resolution honours ordinary member visibility (section 13). The slot identifier names a member of the pattern's type, so a private field of a record is refused when the pattern head is a type alias for that record.
//! cites: 19.3, 13.1
//! expect: reject
//! diagnostic: is private to type
//! issue: 6045
module Conf;

public record Rect
  private h: int;

  public constructor(x: int)
    h := x;
  end constructor;
end Rect;

type R2 := Rect;

public class Program
  public static method Main()
    var r: R2 := new Rect(7);
    case r of R2(h = var v) => Console.WriteLine("y");,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;

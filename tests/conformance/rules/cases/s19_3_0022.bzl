//! case: s19_3_0022
//! doc: spec
//! section: 19.3
//! title: a private field of a record may not be named in a slot over an Object scrutinee
//! rule: Resolution honours ordinary member visibility (section 13). The slot identifier names a member of the pattern's type, so a private field of the record is refused whatever the static type of the scrutinee is, including Object.
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

public class Program
  public static method Main()
    var o: Object := new Rect(7);
    case o of Rect(h = var v) => Console.WriteLine("y");,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;

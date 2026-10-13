//! case: s19_3_0025
//! doc: spec
//! section: 19.3
//! title: a private field may not be named through a module-qualified alias head over an Object scrutinee
//! rule: Resolution honours ordinary member visibility (section 13). The slot identifier names a member of the pattern's type, so a private field of a record is refused when the head is a module-qualified alias for that record, whatever the scrutinee's static type is.
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
    var o: Object := new Rect(7);
    case o of Conf.R2(h = var v) => Console.WriteLine("y");,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;

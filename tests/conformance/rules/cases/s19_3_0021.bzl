//! case: s19_3_0021
//! doc: spec
//! section: 19.3
//! title: a public field of a record named in a slot through a type alias head is accepted
//! rule: Resolution honours ordinary member visibility (section 13), so a public field of the record is named in a slot through a type alias head. This is the accepting twin of the private-field reject: the only change is private to public.
//! cites: 19.3, 13.1
//! expect: run
//! stdout: y
//! issue: 6045
module Conf;

public record Rect
  public h: int;

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

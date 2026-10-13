//! case: s19_3_0028
//! doc: spec
//! section: 19.3
//! title: a private field may be named through an alias head inside the declaring record's own member
//! rule: Resolution honours ordinary member visibility (section 13): the declaring type sees its own private field, so inside its own member an alias head over an Object scrutinee names it and the slot is admitted.
//! cites: 19.3, 13.1
//! expect: run
//! stdout: in7
//! issue: 6045
module Conf;

public record Rect
  private h: int;

  public constructor(x: int)
    h := x;
  end constructor;

  public method Test(): string
    var o: Object := this;
    case o of R2(h = var v) => return "in" + v.ToString();,
      _ => return "o";
    end case;
  end Test;
end Rect;

type R2 := Rect;

public class Program
  public static method Main()
    var r := new Rect(7);
    Console.WriteLine(r.Test());
  end Main;
end Program;

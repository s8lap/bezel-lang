//! case: s19_3_0026
//! doc: spec
//! section: 19.3
//! title: a private field may not be named through a generic alias head over an Object scrutinee
//! rule: Resolution honours ordinary member visibility (section 13). The slot identifier names a member of the pattern's type, so a private field of a generic record is refused when the head is an alias of an instance of it (type BI := Box<int>), whatever the scrutinee's static type is.
//! cites: 19.3, 13.1
//! expect: reject
//! diagnostic: is private to type
//! issue: 6045
module Conf;

public record Box<T>
  private h: T;

  public constructor(x: T)
    h := x;
  end constructor;
end Box;

type BI := Box<int>;

public class Program
  public static method Main()
    var o: Object := new Box<int>(7);
    case o of BI(h = var v) => Console.WriteLine("y");,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;

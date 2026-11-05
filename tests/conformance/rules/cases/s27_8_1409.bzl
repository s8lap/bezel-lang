//! case: s27_8_1409
//! doc: spec
//! section: 27.8
//! title: control: an alias of a generic class instantiation beside generic methods with a parameter T
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8
//! expect: run
//! output: 1
//!   1
module Conf;
public class Stack2<T>
  private owned items: Array<T>;
  public constructor()
    items := new Array<T>();
  end constructor;
  public method Push(x: T)
    items.Add(x);
  end Push;
  public method Count(): int
    return items.Count;
  end Count;
end Stack2;
type IS := Stack2<int>;
public class U
  public static method Id<T>(x: T): T
    return x;
  end Id;
  public static method Wrap<T>(x: T): int
    var l := new Array<T>();
    l.Add(x);
    return l.Count;
  end Wrap;
end U;
public class Program
  public static method Main()
    var s: IS := new Stack2<int>();
    s.Push(U.Id<int>(4));
    Console.WriteLine(s.Count());
    Console.WriteLine(U.Wrap<string>("a"));
  end Main;
end Program;

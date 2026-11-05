//! case: s27_8_1411
//! doc: spec
//! section: 27.8
//! title: control: aliases of a generic class instantiation used at generic method parameters
//! rule: Each instantiation of the alias is interchangeable with the corresponding instantiation of
//!   the aliased type.
//! cites: 27.8
//! expect: run
//! output: 7
//!   7
//!   w
//!   z
module Conf;
public class Box<T>
  private v: T;
  public constructor(x: T)
    v := x;
  end constructor;
  public method Get(): T
    return v;
  end Get;
  public method Other<U>(u: U): U
    return u;
  end Other;
end Box;
type IntBox := Box<int>;
type BoxOf<T> := Box<T>;
public class Program
  public static method Id<T>(b: BoxOf<T>): T
    return b.Get();
  end Id;
  public static method Main()
    var b: IntBox := new Box<int>(7);
    Console.WriteLine(b.Get());
    Console.WriteLine(Id<int>(b));
    var s := new Box<string>("z");
    Console.WriteLine(b.Other<string>("w"));
    Console.WriteLine(Id<string>(s));
  end Main;
end Program;

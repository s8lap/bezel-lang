//! case: s28_3_1404
//! doc: spec
//! section: 28.3
//! title: a using var pin of a field of a temporary at an enclosing-type T: class is rejected
//! rule: A temporary is an admissible borrow source only within its boundary
//! cites: 28.10, 28.1
//! expect: reject
//! diagnostic: lifetime-extended
module Conf;
public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;
public class Box<T>
  public owned V: T;
  public constructor(owned v: T) V := v; end constructor;
end Box;
public class GenQ<T> where T: class
  public static method G(owned t: T): int
    using var x := new Box<T>(move t).V;
    return 1;
  end G;
end GenQ;

public class Program
  public static method Main()
    Console.WriteLine(GenQ<Res>.G(new Res()));
  end Main;
end Program;

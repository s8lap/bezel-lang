//! case: s28_3_1405
//! doc: spec
//! section: 28.3
//! title: a using var pin of a nullable field of a temporary at T: class is rejected
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
public class BoxN<T>
  public owned V: T? := null;
  public constructor(owned v: T) V := v; end constructor;
end BoxN;
public class GenP
  public static method G<T>(owned t: T): int where T: class
    using var x := new BoxN<T>(move t).V;
    return 1;
  end G;
end GenP;

public class Program
  public static method Main()
    Console.WriteLine(GenP.G<Res>(new Res()));
  end Main;
end Program;

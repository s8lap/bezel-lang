//! case: s28_3_1408
//! doc: spec
//! section: 28.3
//! title: a using var pin of a field of a temporary of a non-generic class type is rejected
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
public class GenP
  public static method G(owned t: Res): int
    using var x := new Box<Res>(move t).V;
    return 1;
  end G;
end GenP;

public class Program
  public static method Main()
    Console.WriteLine(GenP.G(new Res()));
  end Main;
end Program;

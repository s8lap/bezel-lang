//! case: s28_3_1410
//! doc: spec
//! section: 28.3
//! title: a using var pin of a field of a borrowed box parameter at T: class is admitted
//! rule: A temporary is an admissible borrow source only within its boundary
//! cites: 28.10, 28.1
//! expect: run
//! output: 1
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
  public static method G<T>(b: Box<T>): int where T: class
    using var x := b.V;
    return 1;
  end G;
end GenP;

public class Program
  public static method Main()
    using var b := new Box<Res>(new Res());
    Console.WriteLine(GenP.G<Res>(b));
  end Main;
end Program;

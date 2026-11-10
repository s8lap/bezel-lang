//! case: s28_3_1411
//! doc: spec
//! section: 28.3
//! title: a using var over a value-typed field of a temporary at T: value is admitted
//! rule: which admits `using var` for any value-type or runtime-managed RHS
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
public class BoxV<T> where T: value
  public V: T;
  public constructor(v: T) V := v; end constructor;
end BoxV;
public class GenP
  public static method G<T>(t: T): int where T: value
    using var x := new BoxV<T>(t).V;
    return 1;
  end G;
end GenP;

public class Program
  public static method Main()
    Console.WriteLine(GenP.G<int>(3));
  end Main;
end Program;

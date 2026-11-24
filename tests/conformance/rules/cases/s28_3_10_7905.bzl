//! case: s28_3_10_7905
//! doc: spec
//! section: 28.3.10
//! title: a lambda's two undischarged owned parameters are disposed in reverse declaration
//!   order
//! rule: undischarged, they are disposed in reverse declaration order, for a method, a
//!   constructor, a lambda and an uncaptured primary-constructor parameter alike
//! cites: 28.3.10
//! expect: run
//! output: 1
//!   21
//!   0
module Conf;
import System;

public class Res
  public static Live: int := 0;
  public static Log: int := 0;
  public Id: int;
  public constructor(i: int)
    Id := i;
    Res.Live := Res.Live + 1;
  end constructor;
  public method V(): int
    return Id * 7;
  end V;
  destructor
    Res.Live := Res.Live - 1;
    Res.Log := Res.Log * 10 + Id;
  end destructor;
end Res;

public class Program
  public static method Main(): int
    var f := lambda(owned a: Res, owned b: Res): int => do
      return 1;
    end;
    var r := f(new Res(1), new Res(2));
    Console.WriteLine(r);
    Console.WriteLine(Res.Log);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;

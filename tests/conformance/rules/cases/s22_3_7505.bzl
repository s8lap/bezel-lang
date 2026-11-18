//! case: s22_3_7505
//! doc: spec
//! section: 22.3
//! title: move at an owned slot of a function value reaches the owned parameter
//! rule: the caller writes the transfer marker at the call site: `M(move x)`
//! cites: 22.3, 27.1
//! expect: run
//! stdout: ok
module Conf;
import System;
class D
  public constructor() end constructor;
end D;
public class Program
  public static method Main(): int
    var d := new D();
    var f: (owned D) -> () := lambda(owned x) => do end;
    f(move d);
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

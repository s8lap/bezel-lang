//! case: s7_5_7732
//! doc: spec
//! section: 7.5
//! title: a null-conditional getter read of a null generic receiver yields null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 14.1
//! expect: run
//! stdout: nul
module Conf;
import System;
public class Box<T>
  public v: T;
  public constructor(a: T) v := a; end constructor;
  public property P: T
    get => do return v; end;
  end P;
end Box;
public static class Program
  public static method Main(args: Array<string>): int
    var bn: Box<string>? := null;
    var y := bn?.P;
    if y = null then Console.WriteLine("nul"); end;
    return 0;
  end Main;
end Program;

//! case: s7_5_7730
//! doc: spec
//! section: 7.5
//! title: a null-conditional read of a getter of a generic class instantiated at string
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 14.1, 23.3
//! expect: run
//! stdout: 5
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
    using var bx := new Box<string>("hello");
    var bn: Box<string>? := bx;
    var y := bn?.P;
    if y <> null then Console.WriteLine(y!.Length); end;
    return 0;
  end Main;
end Program;

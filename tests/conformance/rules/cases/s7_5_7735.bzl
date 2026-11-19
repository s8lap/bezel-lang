//! case: s7_5_7735
//! doc: spec
//! section: 7.5
//! title: a null-conditional chain ending in a call of a function-typed field
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 27.1
//! expect: run
//! stdout: r=5
module Conf;
import System;
public class H
  public F: (int) -> int := lambda(x: int) => x + 1;
  public property Nx: H?
    get => do return this; end;
  end Nx;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    using var h := new H();
    var hn: H? := h;
    var r := hn?.Nx?.F(4);
    if r <> null then Console.WriteLine($"r={r!}"); else Console.WriteLine("null"); end;
    return 0;
  end Main;
end Program;

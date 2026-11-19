//! case: s7_5_7734
//! doc: spec
//! section: 7.5
//! title: a null-conditional call of a unit function-typed field and property runs on a non-null receiver only
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 27.1
//! expect: run
//! output: cb
//!   cb
//!   done
module Conf;
import System;
public class H
  public Cb: () -> () := lambda() => Console.WriteLine("cb");
  public property Q: () -> ()
    get => do return Cb; end;
  end Q;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    using var h := new H();
    var hn: H? := h;
    var hz: H? := null;
    hn?.Cb();
    hn?.Q();
    hz?.Cb();
    hz?.Q();
    Console.WriteLine("done");
    return 0;
  end Main;
end Program;

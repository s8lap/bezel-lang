//! case: s7_5_7726
//! doc: spec
//! section: 7.5
//! title: a null-conditional read of a function-typed block getter yields the getter's closure
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 28.9, 23.3
//! expect: run
//! stdout: 5
module Conf;
import System;
public class H
  private f: () -> int;
  public constructor() f := lambda(): int => 5; end constructor;
  public property P: () -> int
    get => do return f; end;
  end P;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var hn: H? := h;
    var y := hn?.P;
    if y <> null then Console.WriteLine(y!()); end;
    return 0;
  end Main;
end Program;

//! case: s8_7_0003
//! doc: spec
//! section: 8.7
//! title: Dispose on a user class through a null-conditional receiver is accepted
//! rule: "The compiler generates `IDisposable` on every non-static class." (§8.7, v0.340) The user class `D` declares no `Dispose`, yet `d?.Dispose()` over `d: D?` is a call of the generated member and compiles and runs.
//! cites: 28.2
//! expect: run
//! stdout: nc
module Conf;

public class D
  public constructor() end constructor;
end D;

public class Program
  public static method Main()
    var d: D? := new D();
    d?.Dispose();
    Console.WriteLine("nc");
  end Main;
end Program;

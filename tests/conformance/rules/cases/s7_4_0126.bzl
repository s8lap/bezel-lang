//! case: s7_4_0126
//! doc: spec
//! section: 7.4
//! title: A mutably captured local loses its narrowing after a call, so Math.Abs refuses it
//! rule: A variable assigned inside a lambda is mutably captured and "after the first such operation [a call] the narrowing is gone" (§7.4, v0.350). The call `h()` sits between the test and the use, so `Math.Abs(d)` receives a `double?`.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var d: double? := IdD(2.5);
    var h: () -> () := lambda() => do d := IdD(null); end;
    if d <> null then h(); Console.WriteLine(Math.Abs(d)); end;
  end Main;
end Program;

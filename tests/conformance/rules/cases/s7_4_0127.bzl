//! case: s7_4_0127
//! doc: spec
//! section: 7.4
//! title: Math.Max of a mutably captured local is refused after an intervening call
//! rule: Same rule as the Abs form: "any operation that can transfer control to user code" invalidates a mutably captured local (§7.4, v0.350), and an invocation of a function value is one. Generic Max<T> then sees `double?`.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: `Math.Max`
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var d: double? := IdD(2.5);
    var h: () -> () := lambda() => do d := IdD(null); end;
    if d <> null then h(); Console.WriteLine(Math.Max(d, 1.0)); end;
  end Main;
end Program;

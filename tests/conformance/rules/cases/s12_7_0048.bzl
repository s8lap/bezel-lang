//! case: s12_7_0048
//! doc: spec
//! section: 12.7
//! title: an `is` test narrows an `Object` carrier to a stdlib class
//! rule: When the test succeeds and the tested expression is one of [§7.4](#sec-narrowing-applies-to-locals-only)'s narrowing carriers, the carrier is **flow-narrowed** to `T` within the controlled block:
//! cites: 12.7, 8.1, 5.1
//! expect: run
//! output: False
//! issue: 7991
module Conf;
import System.Threading;

public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts;
    if o is CancellationTokenSource then
      Console.WriteLine(o.Token.IsCancellationRequested);
    end if;
    return 0;
  end Main;
end Program;

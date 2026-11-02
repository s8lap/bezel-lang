//! case: s12_7_0047
//! doc: spec
//! section: 12.7
//! title: an `is` test against CancellationTokenSource is answered at an `Object` carrier
//! rule: `is T` tests whether a value is an instance of `T` and produces `bool`.
//! cites: 12.7, 8.1, 5.1
//! expect: run
//! output: True
//! issue: 8000
module Conf;
import System.Threading;

public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts;
    Console.WriteLine(o is CancellationTokenSource);
    return 0;
  end Main;
end Program;

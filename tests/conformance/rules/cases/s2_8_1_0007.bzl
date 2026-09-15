//! case: s2_8_1_0007
//! doc: spec
//! section: 2.8.1
//! title: an implementation-provided module resolves with no artifact supplied
//! rule: The standard-library modules are provided by the implementation, not
//!   by any presented artifact: implicit members of every build's artifact
//!   set, exempt from §2.8.3 rule 1's presentation requirement, resolvable by
//!   `import` with no artifact supplied.
//! cites: 2.8.1, 2.8.3, 2.3
//! expect: run
//! stdout: True
module Conf;
import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    Console.WriteLine(cts.IsCancellationRequested);
  end Main;
end Program;

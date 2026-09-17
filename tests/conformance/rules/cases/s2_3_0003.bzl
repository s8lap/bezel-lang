//! case: s2_3_0003
//! doc: spec
//! section: 2.3
//! title: importing System.Threading makes CancellationTokenSource visible
//! rule: All other library modules require explicit `import`.
//!   `CancellationTokenSource` needs `import System.Threading;` — supplying
//!   it makes the name resolve and the file compile.
//! cites: 2.3
//! expect: run
//! stdout: False
module Conf;
import System.Threading;

public class Program
  public static method Main()
    using var src := new CancellationTokenSource();
    Console.WriteLine(src.IsCancellationRequested);
  end Main;
end Program;

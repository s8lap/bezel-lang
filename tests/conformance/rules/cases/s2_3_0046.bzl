//! case: s2_3_0046
//! doc: spec
//! section: 2.3
//! title: a qualified Mutex with its module imported is accepted
//! rule: `import` makes the public names of the named module visible in
//!   the current file (v0.338, §2.2).
//! cites: 2.3, 2.2
//! expect: run
//! output: ok
module Conf;
import System.Threading;

public class B
  public constructor() end constructor;
  public method Take(m: System.Threading.Mutex): int => 1;
end B;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

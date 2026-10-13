//! case: s2_2_0038
//! doc: spec
//! section: 2.2
//! title: a CancellationToken reached through an import alias is accepted
//! rule: `import` makes the public names of the named module visible in
//!   the current file (v0.338, §2.2).
//! cites: 2.2
//! expect: run
//! output: ok
module Conf;
import System.Threading as T;

public class B
  public constructor() end constructor;
  public method Take(t: T.CancellationToken): int => 1;
end B;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

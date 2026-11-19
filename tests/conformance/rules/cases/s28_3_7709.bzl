//! case: s28_3_7709
//! doc: spec
//! section: 28.3
//! title: the result of invoking a capture-free closure is admitted at an Object parameter
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: run
//! stdout: 1
module Conf;
import System;
public class Sink
  public constructor() end constructor;
  public method Take(o: Object): int return 1; end Take;
end Sink;
public class Program
  public static method Main(): int
    using var s := new Sink();
    var mk := lambda() => lambda() => 3;
    Console.WriteLine(s.Take(mk()));
    return 0;
  end Main;
end Program;

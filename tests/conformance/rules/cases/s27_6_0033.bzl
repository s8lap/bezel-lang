//! case: s27_6_0033
//! doc: spec
//! section: 27.6
//! title: a method reference over an interface-typed head holding a record forms and is called through
//! rule: "Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers)" (§27.6, v0.340). The head `t`
//!   is interface-typed, so the formation captures the instance by reference; the
//!   record's member answers its constant, so the two calls through `f` answer 7
//!   twice. The by-copy capture rule is discriminated by the function-value
//!   equality case `s27_7_1_0009`, which is the observable that tells the two
//!   captures apart.
//! cites: 27.6
//! expect: run
//! output: 7
//!   7
module Conf;

import System;

public interface IThing
  method Bump(): int;
end IThing;

public record RThing: IThing
  public method Bump(): int
    return 7;
  end Bump;
end RThing;

public class Program
  public static method Main()
    var t: IThing := new RThing();
    var f: () -> int := t.Bump;
    Console.WriteLine(f());
    Console.WriteLine(f());
  end Main;
end Program;

//! case: s28_3_8_7203
//! doc: spec
//! section: 28.3.8
//! title: a capture-free function value as the receiver of an extension
//!   Object call is admitted
//! rule: At every set-erasing position a function value with a non-empty
//!   source set is rejected, and a capture-free one is admitted
//! cites: 28.3, 26.5
//! expect: run
//! stdout: 1
module Conf;

public class Res
  public constructor()
  end constructor;

  public method Use(): int => 3;

  public static method Stat(): int => 5;
end Res;

public extension Object
  public method Hold(): int => 1;
end extension;

public class Program
  public static method Main()
    var f: () -> int := lambda() => 2;
    Console.WriteLine(f.Hold());
  end Main;
end Program;

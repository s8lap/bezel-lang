//! case: s28_3_8_7204
//! doc: spec
//! section: 28.3.8
//! title: a static method reference as the receiver of an extension Object
//!   call is admitted
//! rule: a static method reference carries the empty set
//! cites: 28.3, 27.6, 26.5
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
    var f: () -> int := Res.Stat;
    Console.WriteLine(f.Hold());
  end Main;
end Program;

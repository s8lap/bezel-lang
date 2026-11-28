//! case: s27_6_3603
//! doc: spec
//! section: 27.6
//! title: a type-qualified name over a static and an instance method of one name reads the static
//! rule: Where `T` declares both a static member and an instance method named `M`, `T.M` therefore reads
//!   the static member.
//! cites: 27.6, 26.5, 11.3
//! expect: run
//! stdout: 7
//! issue: 8065
module Conf;

public class K
  public constructor() end constructor;
  public static method M(): int => 7;
  public method M(x: int): int => x;
end K;

public class Program
  public static method Main()
    Console.WriteLine(K.M());
  end Main;
end Program;

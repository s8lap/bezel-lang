//! case: s28_3_8_7201
//! doc: spec
//! section: 28.3.8
//! title: a capture-bearing function value as the receiver of an extension
//!   Object call is rejected
//! rule: as the receiver of an extension call whose head is `Object`
//! cites: 28.3, 26.5, 3.1
//! expect: reject
//! diagnostic: receiver
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
    using var payload := new Res();
    var f: () -> int := lambda() => payload.Use();
    Console.WriteLine(f.Hold());
  end Main;
end Program;

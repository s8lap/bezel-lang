//! case: s27_5_8717
//! doc: spec
//! section: 27.5
//! title: a generic class indexer read with a lambda index argument that captures the receiver
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4, 14.1
//! expect: run
//! stdout: 2000
//! notes: Pins #9099 (generic receiver). `Box<string>`'s indexer is read with a lambda that
//!   captures the receiver `b`: q() + Tag = 5 + 5 = 10, 200 times: 2000.
module Conf;
public class Box<T>
  public Tag: int;
  public virtual property [q: () -> int]: int => q() + Tag;
  public constructor() Tag := 5; end constructor;
end Box;
public class Program
  public static method Main(): int
    var t := 0;
    var b := new Box<string>();
    for var j in 0 ..< 200 do
      t := t + b[lambda() => b.Tag];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;

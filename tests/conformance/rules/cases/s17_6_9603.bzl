//! case: s17_6_9603
//! doc: spec
//! section: 17.6
//! title: a user-declared FrozenArray class keeps a writable Length field
//! rule: The operand must be an assignable lvalue (a local variable, a parameter, a
//!   writable field, an indexer slot, or a property with a setter
//! cites: 17.6, 2.3
//! expect: run
//! stdout: 5
//! issue: 9214
module Conf;

public class FrozenArray<T>
  public Length: int;

  public constructor()
    this.Length := 1;
  end constructor;
end FrozenArray;

public class Program
  public static method Main()
    var f := new FrozenArray<int>();
    f.Length := 3;
    f.Length += 1;
    f.Length++;
    Console.WriteLine(f.Length);
  end Main;
end Program;

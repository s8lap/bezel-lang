//! case: l4_19_0006
//! doc: stdlib
//! section: 4.19
//! title: a static owned readonly FrozenArray<T> is a sound all-domains singleton
//! rule: A static owned readonly FrozenArray<T> is a sound all-domains
//!   singleton and draws no rule-4 warning -- the set's first member
//!   admitted by deep immutability rather than synchronization machinery.
//! cites: 4.19, 22.12
//! expect: run
//! stdout: 2
module Conf;

public class Registry
  private static owned readonly _names: FrozenArray<string> := FrozenArray<string>.From(["a", "b"]);

  public static method Names(): FrozenArray<string> => _names;
end Registry;

public class Program
  public static method Main()
    Console.WriteLine(Registry.Names().Length);
  end Main;
end Program;

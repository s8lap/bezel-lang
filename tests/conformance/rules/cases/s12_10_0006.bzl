//! case: s12_10_0006
//! doc: spec
//! section: 12.10
//! title: an acyclic chain over both edge kinds is accepted and dispatches
//! rule: An inheritance cycle is a compile error only where the sequence
//!   returns to its start; a chain over base-class edges and
//!   extended-interface edges that never returns to its start is admitted.
//! cites: 12.10, 11.1, 11.2
//! expect: run
//! stdout: 3
module Conf;

public interface IBase
  method Ping(): int;
end IBase;

public interface IMid: IBase
  method Pong(): int;
end IMid;

public class Root
  public constructor()
  end constructor;
end Root;

public class Leaf: Root, IMid
  public constructor()
  end constructor;

  public method Ping(): int => 1;

  public method Pong(): int => 2;
end Leaf;

public class Program
  public static method Main()
    using var leaf := new Leaf();
    Console.WriteLine(leaf.Ping() + leaf.Pong());
  end Main;
end Program;

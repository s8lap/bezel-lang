//! case: s15_6_0002
//! doc: spec
//! section: 15.6
//! title: slicing is not admitted on OwningArray<T>
//! rule: Slicing is not admitted on `OwningArray<T>` — the copy-returning
//!   semantics would manufacture a second owner for each owned element
//!   (§15.6).
//! cites: 15.6
//! expect: reject
//! diagnostic: slicing
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Program
  public static method Main()
    using var pool: OwningArray<Connection> := [new Connection(), new Connection()];
    var s := pool[0..0];
  end Main;
end Program;

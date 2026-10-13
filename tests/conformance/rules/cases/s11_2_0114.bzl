//! case: s11_2_0114
//! doc: spec
//! section: 11.2
//! title: a read-write property satisfies a read-only interface property of the same name and type
//! rule: `readonly property Value: int;` is the interface member; §11.2 states: "A read-write member satisfies a read-only interface member of the same name and type."
//!   The implementation declares `property Value: int := 5;`, which is read-write, and the match is on name and type.
//! cites: 11.2
//! expect: run
//! stdout: 5
module Conf;

public interface I
  readonly property Value: int;
end I;

public class K: I
  public constructor() end constructor;

  public property Value: int := 5;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.Value);
    return 0;
  end Main;
end C;

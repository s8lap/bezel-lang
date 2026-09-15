//! case: s17_12_0001
//! doc: spec
//! section: 17.12
//! title: throw expr is valid in any expression position — a case arm
//! rule: "`throw expr` raises the supplied exception ... Valid in any
//!   expression position." Worked example: an indexer's `case` arm ending
//!   `_ => throw new IndexOutOfRangeException()`.
//! cites: 17.12
//! expect: run
//! stdout: 20
module Conf;

public class Coords
  public constructor()
  end constructor;

  public property [i: int]: int => (case i of
    0 => 10,
    1 => 20,
    _ => throw new IndexOutOfRangeException());
end Coords;

public class Program
  public static method Main()
    using var c := new Coords();
    Console.WriteLine(c[1]);
  end Main;
end Program;

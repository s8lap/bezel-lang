//! case: s5_1_0145
//! doc: spec
//! section: 5.1
//! title: a long argument at an int parameter is rejected
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost — the conversion table lists no
//!   `long` → `int` row, and a `long` does not fit an `int` without loss.
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method F(x: int): int => x;

  public static method Main(): int
    var j: long := 1;
    return Program.F(j);
  end Main;
end Program;

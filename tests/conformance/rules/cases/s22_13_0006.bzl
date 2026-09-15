//! case: s22_13_0006
//! doc: spec
//! section: 22.13
//! title: a cyclic static-initialization dependency between two types is a compile error
//! rule: The compiler builds a static-initialization dependency graph
//!   over distinct types ... A cycle in this graph is a compile error
//!   naming the participating types.
//! cites: 22.13
//! expect: reject
//! diagnostic: cycle
module Conf;

public class A
  private static _x: int := B.Y();

  public static method X(): int => 1;
end A;

public class B
  private static _y: int := A.X();

  public static method Y(): int => 1;
end B;

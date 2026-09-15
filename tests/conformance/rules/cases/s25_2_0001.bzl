//! case: s25_2_0001
//! doc: spec
//! section: 25.2
//! title: equality may not be overloaded on a class
//! rule: `=` and `<>` may not be overloaded on classes or on inline arrays. For
//!   classes, `=` always invokes `Equals`.
//! cites: 25.2, 8.4
//! expect: reject
//! diagnostic: overload
module Conf;

public class Node
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;

  public static method operator =(a: Node, b: Node): bool => a.Id = b.Id;
  public static method operator <>(a: Node, b: Node): bool => a.Id <> b.Id;
end Node;

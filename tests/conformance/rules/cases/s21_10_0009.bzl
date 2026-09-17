//! case: s21_10_0009
//! doc: spec
//! section: 21.10
//! title: a local type may not reference an enclosing generic parameter
//! rule: A local type may not reference an enclosing generic parameter (an
//!   enclosing method's or class's `T`) — a compile error naming the rule.
//! cites: 21.10, C.1
//! expect: reject
//! diagnostic: generic parameter
module Conf;

public class Box<T>
  public static method Describe(value: T): int
    record Cell(Item: T, Tag: int);
    var c := new Cell(value, 7);
    return c.Tag;
  end Describe;
end Box;

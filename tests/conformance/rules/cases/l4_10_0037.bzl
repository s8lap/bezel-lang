//! case: l4_10_0037
//! doc: stdlib
//! section: 4.10
//! title: Set<T>.Remove result is bool and does not convert to int
//! rule: method Remove(item: T): bool; (stdlib §4.10 Set<T>, ICollection<T>)
//! cites: 4.10
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var s := new Set<int>();
    s.Add(1);
    var n: int := s.Remove(1);
  end Main;
end Program;

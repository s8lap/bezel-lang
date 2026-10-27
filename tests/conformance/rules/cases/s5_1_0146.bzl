//! case: s5_1_0146
//! doc: spec
//! section: 5.1
//! title: a string index at a generic indexer is rejected
//! rule: An indexer is a property whose name is replaced by `[parameters]`, and
//!   the index argument must convert to the indexer's parameter type: a value
//!   of one type implicitly converts to another when no programmer-visible
//!   information is lost — a `string` is not among `int`'s implicit sources.
//! cites: 5.1, 23.4
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7064
module Conf;

public class GBox<T>
  public property [i: T]: T => i;
end GBox;

public class Program
  public static method Main(): int
    var b := new GBox<int>();
    var v := b["x"];
    Console.WriteLine(v.ToString());
    return 0;
  end Main;
end Program;

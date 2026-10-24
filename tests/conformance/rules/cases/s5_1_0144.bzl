//! case: s5_1_0144
//! doc: spec
//! section: 5.1
//! title: a double index at a user-defined indexer is rejected
//! rule: An indexer is a property whose name is replaced by `[parameters]`, and
//!   the index argument must convert to the indexer's parameter type: a value
//!   of one type implicitly converts to another when no programmer-visible
//!   information is lost — `double` is not among `int`'s implicit sources.
//! cites: 5.1, 23.4
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7064
module Conf;

public class UBox
  public property [i: int]: int => i * 2;
end UBox;

public class Program
  public static method Main(): int
    var b := new UBox();
    Console.WriteLine(b[1.5]);
    return 0;
  end Main;
end Program;

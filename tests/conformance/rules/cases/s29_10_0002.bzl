//! case: s29_10_0002
//! doc: spec
//! section: 29.10
//! title: a body with no yield anywhere is an ordinary method, not an iterator
//! rule: An empty body (no `yield`) is not an iterator method.
//! cites: 29.10
//! expect: accept
module Conf;

public class Program
  public static method Ids(): IEnumerable<int>
    return new Array<int>();
  end Ids;
end Program;

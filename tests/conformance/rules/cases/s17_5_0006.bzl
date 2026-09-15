//! case: s17_5_0006
//! doc: spec
//! section: 17.5
//! title: not x in c parses as (not x) in c and is a type error for a non-bool x
//! rule: "`not x in c` parses as `(not x) in c`, a type error for a non-`bool`
//!   `x`."
//! cites: 17.5, 17.1
//! expect: reject
//! diagnostic: bool
module Conf;

public class Program
  public static method Check(numbers: Array<int>): bool
    var r := not 5 in numbers;
    return r;
  end Check;
end Program;

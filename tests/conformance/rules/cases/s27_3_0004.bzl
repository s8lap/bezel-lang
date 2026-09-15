//! case: s27_3_0004
//! doc: spec
//! section: 27.3
//! title: a block-bodied lambda where one path returns a value and another does not is rejected
//! rule: A body where one path returns a value and another exits without one
//!   is a compile error, the same totality rule as for methods.
//! cites: 27.3
//! expect: reject
//! diagnostic: exit path
module Conf;

public class Program
  public method Demo()
    var classify: (int) -> string := lambda(x: int): string => do
      if x > 0 then
        return "pos";
      end;
    end;
  end Demo;
end Program;

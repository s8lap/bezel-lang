//! case: s26_5_0041
//! doc: spec
//! section: 26.5
//! title: an extension StringBuilder.Append with an arity no instance Append has is not reached
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5
//! expect: reject
//! diagnostic: the call to `StringBuilder.Append`
module Conf;

extension StringBuilder
  public method Append(a: int, b: int, c: int): StringBuilder
    this.Append("X");
    return this;
  end Append;
end;

public class Program
  public static method Main()
    var sb := new StringBuilder("r");
    sb.Append(1, 2, 3);
  end Main;
end Program;

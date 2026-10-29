//! case: s26_5_0042
//! doc: spec
//! section: 26.5
//! title: an extension on Object named Append is not reached for a StringBuilder receiver
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5
//! expect: reject
//! diagnostic: the call to `StringBuilder.Append`
module Conf;

extension Object
  public method Append(x: int, y: int): string => "EXT";
end;

public class Program
  public static method Main()
    var sb := new StringBuilder("ab");
    Console.WriteLine(sb.Append(1, 2));
  end Main;
end Program;

//! case: s22_6_0102
//! doc: spec
//! section: 22.6
//! title: an Array<int> argument to Encoding.GetString matches none of its byte-buffer overloads
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, ... — the call is a compile
//!   error with no candidates cited, because no overload was reachable.
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: Encoding.GetString
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [72, 105];
    Console.WriteLine(Encoding.UTF8.GetString(a));
  end Main;
end Program;

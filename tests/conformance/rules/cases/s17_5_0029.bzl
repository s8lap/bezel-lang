//! case: s17_5_0029
//! doc: spec
//! section: 17.5
//! title: a key tests membership in a Dictionary through its Contains(key)
//! rule: "The `Contains` lookup is structural rather than interface-based: any
//!   accessible method named `Contains` taking one parameter of compatible
//!   type and returning `bool` qualifies" ([§17.5]). The Dictionary's
//!   `Contains` takes a key.
//! cites: 17.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["a"] := 1;
    var found := "a" in d;
    Console.WriteLine(found);
  end Main;
end Program;

//! case: s17_5_0035
//! doc: spec
//! section: 17.5
//! title: a constant at the top of the byte range converts to the Set<byte> element and is found
//! rule: "A constant whose value does not fit the target's range is a **compile error** at the conversion site" ([§5.1]) only when it does not fit; a constant whose value fits "implicitly converts to any integer type whose range contains its value" ([§5.1]), so `255` is admissible as `Contains(item: byte)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var s: Set<byte> := [1, 255];
    var found := 255 in s;
    Console.WriteLine(found);
  end Main;
end Program;

//! case: s1_3_0002
//! doc: spec
//! section: 1.3
//! title: identifiers are case-sensitive
//! rule: Identifiers are case-sensitive: `Foo`, `foo`, and `FOO` are distinct.
//! expect: run
//! stdout: 123
module Conf;

public class Program
  public static method Main()
    var foo := 1;
    var Foo := 2;
    var FOO := 3;
    Console.WriteLine($"{foo}{Foo}{FOO}");
  end Main;
end Program;

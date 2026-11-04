//! case: s26_1_0006
//! doc: spec
//! section: 26.1
//! title: `this` in an extension over `String` reaches int.Parse as the receiver string
//! rule: the dual-form rule above makes the PascalCase form interchangeable with the keyword everywhere a type is named
//! cites: 3.4, 26.1, 9.1
//! expect: run
//! issue: 7304
//! stdout: 42

module Conf;

extension String
  public method P(): int => int.Parse(this);
end;

public class Program
  public static method Main()
    Console.WriteLine("41".P() + 1);
  end Main;
end Program;

//! case: s26_1_0007
//! doc: spec
//! section: 26.1
//! title: `this` in an extension headed by an alias of string reaches int.Parse as the receiver string
//! rule: The head is the alias's expansion
//! cites: 26.1, 27.8, 9.1
//! expect: run
//! issue: 7304
//! stdout: 42

module Conf;

type Name := string;

extension Name
  public method P(): int => int.Parse(this);
end;

public class Program
  public static method Main()
    Console.WriteLine("41".P() + 1);
  end Main;
end Program;

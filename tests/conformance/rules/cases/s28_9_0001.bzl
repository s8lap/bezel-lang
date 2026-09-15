//! case: s28_9_0001
//! doc: spec
//! section: 28.9
//! title: a getter returning a freshly-computed string needs no owned announcement
//! rule: Runtime-managed types are exempt from the ceremony this chapter
//!   states for class-typed values; a getter body whose result is a fresh
//!   runtime-managed production (an interpolation, a concatenation) remains
//!   admissible with no `owned` announcement.
//! cites: 28.9, 28.6
//! expect: run
//! stdout: hello world
module Conf;

public class Greeter
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  public property Greeting: string => $"hello {_name}";
end Greeter;

public class Program
  public static method Main()
    var g := new Greeter("world");
    Console.WriteLine(g.Greeting);
  end Main;
end Program;

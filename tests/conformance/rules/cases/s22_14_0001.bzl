//! case: s22_14_0001
//! doc: spec
//! section: 22.14
//! title: a variadic-style signature uses Array<T> with a collection literal at the call site
//! rule: There is no `params` keyword. A variadic-style signature uses
//!   `Array<T>` with a collection literal at the call site.
//! cites: 22.14
//! expect: run
//! stdout: hello
module Conf;

public class Logger
  public static method Log(messages: Array<string>)
    for var m in messages do
      Console.WriteLine(m);
    end;
  end Log;
end Logger;

public class Program
  public static method Main()
    Logger.Log(["hello", "world"]);
  end Main;
end Program;

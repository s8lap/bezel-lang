//! case: s29_9_0001
//! doc: spec
//! section: 29.9
//! title: yield is a method-level construct and is barred inside a lambda
//! rule: `yield` is a method-level construct only.
//! cites: 29.9
//! expect: reject
//! diagnostic: yield
module Conf;

public class Program
  public static method Main()
    var f: () -> () := lambda() => do
      Console.WriteLine("body");
      yield return 1;
    end;
    f();
  end Main;
end Program;

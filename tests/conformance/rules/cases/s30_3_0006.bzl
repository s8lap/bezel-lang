//! case: s30_3_0006
//! doc: spec
//! section: 30.3
//! title: Result on a completed Task<T> for class T is a borrow sourced at the task
//! rule: Reading `Result` on a completed `Task<T>` whose `T` is a **class**
//!   type (Pattern A) is additionally subject to the same owning-source
//!   rule as `await`: the read produces a **borrow whose source is the task
//!   expression's location**.
//! cites: 30.3, 30.5
//! expect: run
//! stdout: hi
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Loader
  public async method LoadAsync(): Task<Widget>
    await Task.Delay(0);
    return new Widget();
  end LoadAsync;
end Loader;

public class Program
  public static method Main()
    using var t := Loader.LoadAsync();
    Console.WriteLine(t.Result.Greet());
  end Main;
end Program;

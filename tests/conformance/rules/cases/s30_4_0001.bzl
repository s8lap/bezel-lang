//! case: s30_4_0001
//! doc: spec
//! section: 30.4
//! title: a Task<T> may be awaited from multiple places
//! rule: A `Task` or `Task<T>` may be awaited from multiple places,
//!   sequentially or concurrently. Every awaiter sees the same eventual
//!   state. The underlying computation runs once.
//! cites: 30.4
//! expect: run
//! stdout: hi
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Loader
  public static async method LoadAsync(): Task<Widget>
    await Task.Delay(0);
    return new Widget();
  end LoadAsync;
end Loader;

public class Program
  public static async method Main(): Task
    using var t := Loader.LoadAsync();
    var w1 := await t;
    var w2 := await t;
    Console.WriteLine(w1.Greet());
  end Main;
end Program;

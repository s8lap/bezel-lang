//! case: s30_3_0005
//! doc: spec
//! section: 30.3
//! title: OwningTask<T>.Result on a completed task transfers ownership
//! rule: `OwningTask<T>` exposes `Result`: reading it on a **completed**
//!   task in an owned-consuming position transfers ownership of the result
//!   to the reader exactly as `await` does.
//! cites: 30.3, 30.5
//! expect: run
//! stdout: hi
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Program
  public static method Main()
    using var t := Task.FromOwnedResult<Widget>(new Widget());
    using var w := t.Result;
    Console.WriteLine(w.Greet());
  end Main;
end Program;

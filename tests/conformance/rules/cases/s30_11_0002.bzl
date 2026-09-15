//! case: s30_11_0002
//! doc: spec
//! section: 30.11
//! title: OwningTaskCompletionSource<T> transfers ownership of a class-typed result
//! rule: For class T ... the source takes ownership of the result on
//!   completion, and the eventual awaiter takes ownership through the
//!   `using var x := await ...` shape.
//! cites: 30.11
//! expect: run
//! stdout: hi
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;

  public method Greet(): string => "hi";
end Widget;

public class Program
  public static async method LoadAsync(): OwningTask<Widget>
    using var tcs := new OwningTaskCompletionSource<Widget>();
    tcs.SetResult(new Widget());
    return await tcs.Task;
  end LoadAsync;

  public static async method Main(): Task
    using var w := await LoadAsync();
    Console.WriteLine(w.Greet());
  end Main;
end Program;

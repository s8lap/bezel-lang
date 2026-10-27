//! case: s28_12_0029
//! doc: spec
//! section: 28.12
//! title: plain using at an IDisposable static type runs the run-time object's destructor
//! rule: `await using` selects the logical-disposal path by the class of the object
//!   the binding holds — as the synchronous path selects the destructor chain
//!   it runs ([§28.4](#sec-the-destructor)) — never by the binding's static
//!   type.
//! cites: 28.12, 28.4
//! expect: run
//! output: body
//!   SYNC

module Conf;

import System.Threading;

public class Solo: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("ASYNC");
  end DisposeAsync;

  destructor
    Console.WriteLine("SYNC");
  end destructor;
end Solo;

public class Program
  public static method Main()
    using var d: IDisposable := new Solo();
    Console.WriteLine("body");
  end Main;
end Program;

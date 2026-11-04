//! case: s30_15_0008
//! doc: spec
//! section: 30.15
//! title: over a value-typed element Send takes a plain parameter and Receive is a Task<T>
//! rule: **`Send(owned item: T)`** (class-typed T; for sendable value types,
//!   `string` and `Type` the parameter is unmodified) appends the item …
//!   **`Receive()`** is asynchronous — `OwningTask<T>` for class-typed T
//!   (Pattern B: awaiting transfers ownership of the item to the receiver),
//!   `Task<T>` otherwise.
//! cites: 30.15, 30.5
//! expect: run
//! stdout: 7
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();
    using var tx := ch.NewSender();
    var n := 7;
    tx.Send(n);
    using var r: Task<int> := ch.Receive();
    var got := await r;
    Console.WriteLine(got);
  end Main;
end Program;

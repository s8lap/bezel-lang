//! case: l8_1_0003
//! doc: stdlib
//! section: 8.1
//! title: Stopwatch is sendable and a moved running stopwatch keeps running
//! rule: `Stopwatch` is **sendable**, on the sendable form's copy-state
//!   ground. A moved running stopwatch keeps measuring.
//! cites: 8.1, 1, 6.9
//! expect: run
//! stdout: True
//! issue: 5587
module Conf;

import System.Diagnostics;
import System.Threading;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<Stopwatch>();
    using var tx := inbox.NewSender();
    var sw := Stopwatch.StartNew();
    tx.Send(move sw);
    using var got := await inbox.Receive();
    Console.WriteLine(got.IsRunning);
  end Main;
end Program;

//! case: l3_1_0006
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder is sendable and a moved builder keeps its contents
//! rule: `StringBuilder` is **sendable**, on the sendable form's copy-state
//!   ground. A moved builder keeps its contents.
//! cites: 3.1, 1, 6.9
//! expect: run
//! stdout: kept
//! issue: 5587
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<StringBuilder>();
    using var tx := inbox.NewSender();
    var sb := new StringBuilder();
    sb.Append("kept");
    tx.Send(move sb);
    using var got := await inbox.Receive();
    Console.WriteLine(got.ToString());
  end Main;
end Program;

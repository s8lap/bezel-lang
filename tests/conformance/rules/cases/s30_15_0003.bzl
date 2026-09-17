//! case: s30_15_0003
//! doc: spec
//! section: 30.15
//! title: Send transfers ownership into the channel and Receive transfers it out
//! rule: A **channel** carries sendable values from one domain to another
//!   with ownership transfer at both ends: `Send` consumes the value in the
//!   sending domain; `Receive` produces it owned in the receiving domain.
//! cites: 30.15
//! expect: run
//! stdout: ok
module Conf;

import System.Threading;

public sealed class Report
  public constructor()
  end constructor;

  public method Describe(): string => "ok";
end Report;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<Report>();
    using var tx := inbox.NewSender();
    tx.Send(new Report());
    using var r := await inbox.Receive();
    Console.WriteLine(r.Describe());
  end Main;
end Program;

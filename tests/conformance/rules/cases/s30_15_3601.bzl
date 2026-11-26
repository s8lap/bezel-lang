//! case: s30_15_3601
//! doc: spec
//! section: 30.15
//! title: a Receive pending when the channel is closed faults with ChannelClosedException
//! rule: A `Receive` pending on an empty channel when the channel closes — by `Close()`, by the last
//!   sender's disposal, or by the channel's own disposal — has its task faulted with
//!   `ChannelClosedException`
//! cites: 30.15
//! expect: run
//! stdout: CCE
//! issue: 8710
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();
    using var tx := ch.NewSender();
    using var rx := ch.Receive();
    ch.Close();
    try
      var v := await rx;
      Console.WriteLine(v);
    catch
      ChannelClosedException => Console.WriteLine("CCE"),
    end try;
  end Main;
end Program;

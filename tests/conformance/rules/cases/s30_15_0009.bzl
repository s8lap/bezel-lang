//! case: s30_15_0009
//! doc: spec
//! section: 30.15
//! title: TryReceive while a ReceiveAll enumeration is active raises InvalidOperationException
//! rule: At most one consumption may be in progress (the channel is
//!   single-consumer; this names the obligation): … calling
//!   `Receive`/`TryReceive` while a `ReceiveAll` enumeration is active, raise
//!   `InvalidOperationException`.
//! cites: 30.15
//! expect: run
//! output: 1
//!   refused
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();
    using var tx := ch.NewSender();
    tx.Send(1);
    tx.Send(2);
    await for var x in ch.ReceiveAll() do
      Console.WriteLine(x);
      try
        Console.WriteLine(ch.TryReceive(out _));
      catch
        InvalidOperationException => Console.WriteLine("refused"),
      end try;
      break;
    end;
  end Main;
end Program;

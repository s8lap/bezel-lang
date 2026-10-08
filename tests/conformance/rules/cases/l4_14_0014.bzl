//! case: l4_14_0014
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer<double> is sendable and its storage crosses with it
//! rule: `OwningBuffer<T>` is sendable iff its element type is a sendable
//!   class type or a sendable value type: its storage is part of the
//!   instance and crosses with it (`OwningBuffer<double>` is sendable; a
//!   buffer of a borrow-bearing record is not).
//! cites: 4, 4.14, 30.14, 6.9
//! expect: run
//! stdout: 2.5
//! issue: 5587
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<OwningBuffer<double>>();
    using var tx := inbox.NewSender();
    var buf := new OwningBuffer<double>(2);
    buf[0] := 2.5;
    tx.Send(move buf);
    using var got := await inbox.Receive();
    Console.WriteLine(got[0]);
  end Main;
end Program;

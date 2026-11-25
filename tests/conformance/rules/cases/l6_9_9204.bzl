//! case: l6_9_9204
//! doc: stdlib
//! section: 6.9
//! title: Close() mid-drain with two SendAsync producers: accepted items arrive, drain completes
//! rule: buffered items remain receivable, an active `ReceiveAll` completes normally after the tail
//! expect: run
//! output: good 100 bad 0
//! notes: Pins #9054 (fix 3914ee801). The consumer calls Close() at the 7th item; later SendAsync calls fault
//!   with ChannelClosedException (not accepted). Items accepted before the close stay receivable, so the
//!   received count equals the sum of the producers' accepted counts, every round (100 rounds, capacity
//!   1..5).
module Conf;
import System.Threading;
public static class Producers
  public static async method Produce(owned tx: ChannelSender<int>): Task<int>
    using var sender := move tx;
    var accepted := 0;
    for var i in 1..20 do
      var isclosed := false;
      try
        await sender.SendAsync(1);
        accepted += 1;
      catch
        ChannelClosedException => do isclosed := true; end
      end try;
      if isclosed then return accepted; end if;
    end for;
    return accepted;
  end Produce;
  public static async method Round(cap: int): Task<int>
    using var ch := new Channel<int>(cap);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Produce(move s1);
    using var p2 := Produce(move s2);
    var got := 0;
    await for var v in ch.ReceiveAll() do
      got += 1;
      if got = 7 then ch.Close(); end if;
    end for;
    var a := await p1;
    var b := await p2;
    if got = a + b then return 1; end if;
    return 0;
  end Round;
end Producers;
public class Program
  public static async method Main(): Task
    var good := 0;
    var bad := 0;
    for var round in 1..100 do
      if await Producers.Round(1 + round mod 5) = 1 then good += 1; else bad += 1; end if;
    end for;
    Console.WriteLine($"good {good} bad {bad}");
  end Main;
end Program;

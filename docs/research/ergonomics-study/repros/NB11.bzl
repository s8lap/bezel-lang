// bug: NB11
// title: a bounded channel with `SendAsync` and two producers loses items and hangs
// issue: new
// rests-on: stdlib §6.9 / Guide 4 §6: `SendAsync` awaits capacity; each item is received exactly once; disposing the last sender closes the channel
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// note: Line order depends on scheduling, so lines compare as a multiset. The hang is timing-dependent (24 of 25 runs on r42).
// expect: run
// order: any
// stdout: got 1
// stdout: got 2
// stdout: got 3
// stdout: got 10
// stdout: got 20
// stdout: got 30
// stdout: p1 sent 1
// stdout: p1 sent 2
// stdout: p1 sent 3
// stdout: p1 done
// stdout: p10 sent 10
// stdout: p10 sent 20
// stdout: p10 sent 30
// stdout: p10 done
// stdout: sum 66
module P;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..3 do
      await sender.SendAsync(id * i);
      Console.WriteLine($"p{id} sent {id * i}");
    end;
    Console.WriteLine($"p{id} done");
  end Produce;
end Producers;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(1);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    var sum := 0;
    await for var v in ch.ReceiveAll() do Console.WriteLine($"got {v}"); sum += v; end;
    Console.WriteLine($"sum {sum}");
  end Main;
end Program;

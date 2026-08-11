public class Channel<T> where T: sendable                  // the receiving end; class T form (v0.201: the constraint states §6.9's element requirement)
  public constructor();                                    // unbounded
  public constructor(capacity: int);                       // BOUNDED (v0.174, #809): back-pressure;
                                                           //   capacity >= 1, ArgumentException otherwise

  public readonly property Count: int;                     // items currently buffered
  public readonly property IsClosed: bool;                 // true once closed (see below)

  public method NewSender(): owned ChannelSender<T>;       // mint a sender endpoint
  public method Close();                                   // v0.222 (#1807): receiver-initiated graceful
                                                           //   close — same closed state as last-sender
                                                           //   dispose; buffered items remain receivable;
                                                           //   idempotent (no-op when already closed)

  public method Receive(): OwningTask<T>;                   // await transfers ownership of one item
  public method TryReceive(out owned item: T?): bool;      // non-blocking; false when none available
  public method ReceiveAll(): IOwningAsyncEnumerable<T>;  // v0.186: drain form — see below
  destructor;    // closes the channel; disposes every undelivered class-typed
                 //   item in reverse insertion order (language spec §30.15)
end Channel;

public class ChannelSender<T> where T: sendable            // class T form (v0.201)
  public method Send(owned item: T);                       // transfers ownership into the channel;
                                                           //   never blocks; on a FULL bounded channel
                                                           //   raises ChannelFullException (v0.174);
                                                           //   ChannelClosedException if closed
  public async method SendAsync(owned item: T): Task;      // v0.174 (#809): awaits capacity on a
                                                           //   bounded channel (immediate when space
                                                           //   or unbounded); ChannelClosedException
                                                           //   if the channel closes while waiting
  destructor;    // disposing the last sender closes the channel (language spec §30.15)
end ChannelSender;

public sealed class Job                          // sendable: sealed, value fields (spec §30.14)
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public record JobResult(Id: int, Ok: bool);      // sendable value record

public sealed class WorkerCtx                    // Domain.Spawn takes ONE argument (§6.8) → aggregate
  public owned Inbox: Channel<Job>;
  public owned Results: ChannelSender<JobResult>;
  public constructor(owned inbox: Channel<Job>, owned results: ChannelSender<JobResult>)
    Inbox := inbox; Results := results;
  end constructor;
end WorkerCtx;

class Pool
  private static async method WorkerMain(owned ctx: WorkerCtx): Task
    using var c := move ctx;                               // adopt (transfer marker)
    await for var job in c.Inbox.ReceiveAll() do           // drain form: completes normally on close
      using var j := move job;                             // owned iteration variable discharged
      c.Results.Send(new JobResult(j.Id, true));           // value-form Send (record payload)
    end;
  end WorkerMain;                                          // ctx disposal releases the results-sender

  public static async method Main(): Task
    var workers := 4;
    using var results := new Channel<JobResult>();         // boss = the single consumer
    using var senders := new OwningArray<ChannelSender<Job>>();
    using var joins   := new OwningArray<Task>();
    for var i in 0..<workers do
      var inbox := new Channel<Job>(capacity := 8);        // bounded: back-pressure (v0.174)
      senders.Add(inbox.NewSender());
      joins.Add(Domain.Spawn(WorkerMain,
                new WorkerCtx(move inbox, results.NewSender())));   // async-entry row (§6.8)
    end;
    for var n in 0..<100 do
      await senders[n mod workers].SendAsync(new Job(n));  // awaits capacity
    end;
    senders.Clear();                                       // dispose all job-senders → every inbox closes
    var done := 0;
    await for var r in results.ReceiveAll() do done += 1; end;
    for var t in joins.Drain() do
      using var jt := move t;
      await jt;                                            // join; faults surface as DomainFaultedException
    end;
    Console.WriteLine($"done: {done}");
  end Main;
end Pool;

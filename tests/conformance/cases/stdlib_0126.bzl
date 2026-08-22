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

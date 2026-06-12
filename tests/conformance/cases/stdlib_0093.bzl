public class Channel<T>: IDisposable                       // the receiving end; class T form
  public constructor();                                    // unbounded

  public readonly property Count: int;                     // items currently buffered
  public readonly property IsClosed: bool;                 // true once closed (see below)

  public method NewSender(): owned ChannelSender<T>;       // mint a sender endpoint

  public method Receive(): OwnedTask<T>;                   // await transfers ownership of one item
  public method TryReceive(out owned item: T?): bool;      // non-blocking; false when none available
end Channel;

public class ChannelSender<T>: IDisposable                 // class T form
  public method Send(owned item: T);                       // transfers ownership into the channel;
                                                           //   never blocks (unbounded);
                                                           //   ChannelClosedException if closed
end ChannelSender;

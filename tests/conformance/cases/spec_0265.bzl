public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource
  public constructor();
  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method Cancel();
  public method CancelAfter(delay: TimeSpan);
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;
  destructor;       // cancel-on-dispose: signals cancellation to observing tokens
end CancellationTokenSource;

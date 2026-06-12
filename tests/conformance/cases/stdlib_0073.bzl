public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource
  public constructor();

  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;

  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;

  public method Cancel();
  public method CancelAfter(delay: TimeSpan);

  destructor;       // signals cancellation to any tokens still observing,
                           // then releases the registered-callback list
end CancellationTokenSource;

public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource: IDisposable
  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method Cancel();
  public method CancelAfter(delay: TimeSpan);
end CancellationTokenSource;

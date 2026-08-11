public record CancellationToken
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end CancellationToken;

public class CancellationTokenSource
  public constructor();

  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;

  // v0.173 (#810): a source cancelled when ANY parent token cancels (or by its
  // own Cancel/CancelAfter). Two-token form for the everyday case (caller +
  // deadline); array form for N parents.
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;

  public readonly property Token: CancellationToken;
  public readonly property IsCancellationRequested: bool;

  public method Cancel();
  public method CancelAfter(delay: TimeSpan);

  destructor;       // signals cancellation to any tokens still observing,
                           // unregisters from linked parents (v0.173), then
                           // releases the registered-callback list
end CancellationTokenSource;

using var timeoutSrc := CancellationTokenSource.WithCancelAfter(timeout);
using var cts := CancellationTokenSource.CreateLinked(callerToken, timeoutSrc.Token);

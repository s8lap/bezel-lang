public class TaskScope: IAsyncDisposable
  public constructor();                                    // fresh internal CancellationTokenSource
  public readonly property Token: CancellationToken;       // the scope's token — pass to children
  public method CancelAfter(delay: TimeSpan);              // deadline for the whole scope
  public method Cancel();                                  // cancel all children now

  public method Spawn(scoped body: () -> Task);            // start a child (§28.14 storage-bounded);
                                                           //   InvalidOperationException once closed
  public async method Join(): Task;                        // await all children; re-raise the FIRST
                                                           //   fault (first-fault-wins, no aggregation)
  // await using only (see below); DisposeAsync drains and never raises
end TaskScope;

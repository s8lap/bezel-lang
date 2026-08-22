import System.Threading;

public class Refresher
  private static async method RefreshOneAsync(name: string, token: CancellationToken): Task
    await Task.Delay(TimeSpan.FromMilliseconds(10), token);
    Console.WriteLine($"refreshed {name}");
  end RefreshOneAsync;

  public async method RefreshBoth(primary: string, mirror: string): Task
    await using var scope := new TaskScope();    // `await using` only; a plain `using var` is an error
    scope.CancelAfter(TimeSpan.FromSeconds(30));
    scope.Spawn(async lambda() => do
      await RefreshOneAsync(primary, scope.Token);
    end);
    scope.Spawn(async lambda() => do
      await RefreshOneAsync(mirror, scope.Token);
    end);
    await scope.Join();                          // mandatory on the normal path; the single re-raise site
  end RefreshBoth;
end Refresher;

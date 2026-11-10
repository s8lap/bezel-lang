module Errors.Retry;

import System.IO;

public class Flaky
  private _calls: int := 0;
  public async method FetchAsync(): Task<int>
    _calls++;
    await Task.Delay(1);
    if _calls < 3 then
      throw new IOException("transient");
    end;
    return 42;
  end FetchAsync;
end Flaky;

public static class Retry
  public static async method WithRetry<T>(op: () -> Task<T>, maxAttempts: int): Task<T>
    var attempt := 1;
    while true do
      try
        return await op();
      catch
        var ex: IOException when attempt < maxAttempts => do
          Console.WriteLine($"attempt {attempt} failed: {ex.Message}");
        end
      end;
      await Task.Delay(10 * attempt);
      attempt++;
    end;
  end WithRetry;
end Retry;

public class Program
  public static async method Main(): Task
    var f := new Flaky();
    var r := await Retry.WithRetry<int>(lambda() => f.FetchAsync(), 3);
    Console.WriteLine($"result {r}");
    var g := new Flaky();
    try
      var r2 := await Retry.WithRetry<int>(lambda() => g.FetchAsync(), 2);
      Console.WriteLine($"result {r2}");
    catch
      var ex: IOException => Console.WriteLine($"gave up: {ex.Message}")
    end;
  end Main;
end Program;

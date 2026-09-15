//! case: s30_7_0004
//! doc: spec
//! section: 30.7
//! title: await for outside an async method is rejected
//! rule: `await for` is valid only inside an async method or async lambda.
//! cites: 30.7
//! expect: reject
//! diagnostic: await for
module Conf;

public class Reader
  public static async method NumbersAsync(): owned IAsyncEnumerable<int>
    await Task.Delay(0);
    yield return 1;
  end NumbersAsync;
end Reader;

public class Program
  public method RunNow()
    await for var n in Reader.NumbersAsync() do
      Console.WriteLine(n);
    end;
  end RunNow;
end Program;

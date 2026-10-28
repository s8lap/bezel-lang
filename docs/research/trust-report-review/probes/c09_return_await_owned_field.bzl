module Conf;
// Family C: `return await _task;` over an owned field of `this` read without `move` (§30.5 kind 3, non-static source). EXPECT: report.
import System.Threading;
public class Data
  public constructor()
  end constructor;
end Data;
public class Cache
  private owned _task: Task<Data>;
  public constructor(owned t: Task<Data>)
    _task := t;
  end constructor;
  public async method Get(): Task<Data>
    return await _task;
  end Get;
end Cache;

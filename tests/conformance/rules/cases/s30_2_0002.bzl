//! case: s30_2_0002
//! doc: spec
//! section: 30.2
//! title: await outside an async method or lambda is rejected
//! rule: `await` is valid only inside an `async` method or `async` lambda.
//! cites: 30.2
//! expect: reject
//! diagnostic: await
module Conf;

public class Worker
  public method RunNow(): int
    var n := await Task.FromResult(5);
    return n;
  end RunNow;
end Worker;

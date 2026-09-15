//! case: s30_1_0005
//! doc: spec
//! section: 30.1
//! title: an async method returning a plain value type is rejected
//! rule: A method declared `async` may use `await`. The return type must be
//!   one of `Task`, `Task<T>`, `OwningTask<T>`, or one of the async
//!   iterable interfaces — a plain `string` is none of these.
//! cites: 30.1
//! expect: reject
//! diagnostic: return type
module Conf;

public class Worker
  public async method GetNameAsync(): string
    return "x";
  end GetNameAsync;
end Worker;

//! case: s30_1_0001
//! doc: spec
//! section: 30.1
//! title: an async method may return Task<T> for a value-typed T
//! rule: The return type must be one of: `Task` — no result value; `Task<T>`
//!   for some type T admitted by §30.5's producer taxonomy — a result of
//!   type T, with no transferable ownership obligation on the awaiter.
//! cites: 30.1, 30.5
//! expect: accept
module Conf;

public class Loader
  public async method ComputeAsync(): Task<int>
    await Task.Delay(0);
    return 42;
  end ComputeAsync;
end Loader;

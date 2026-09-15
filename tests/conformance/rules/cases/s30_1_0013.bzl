//! case: s30_1_0013
//! doc: spec
//! section: 30.1
//! title: CancellationToken is the sole admitted borrow-bearing async parameter
//! rule: The one exception is a borrow-bearing record the standard library
//!   certifies with a normatively-stated source-outlives-task obligation —
//!   currently only `CancellationToken`.
//! cites: 30.1, 30.8
//! expect: accept
module Conf;

public class Program
  public async method HandleAsync(token: CancellationToken): Task
    await Task.Delay(0);
  end HandleAsync;
end Program;

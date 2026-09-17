//! case: s22_16_0005
//! doc: spec
//! section: 22.16
//! title: await is rejected in a contract clause, even on an async member
//! rule: `await` is rejected, on every member including `async` ones (a
//!   clause is not part of an async body's statement sequence and has
//!   no suspension semantics).
//! cites: 22.16
//! expect: reject
//! diagnostic: await
module Conf;

public class Fetcher
  public static async method ValidateAsync(): Task<bool>
    return true;
  end ValidateAsync;

  public async method Load(): Task<int>
    requires(await ValidateAsync())

    return 1;
  end Load;
end Fetcher;

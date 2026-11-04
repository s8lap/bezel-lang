//! case: s32_8_0016
//! doc: spec
//! section: 32.8
//! title: a function type with an env position is not a Bezel type outside an extern declaration
//! rule: **Type identity**: a function type containing an `env` position is a
//!   *declaration form*, not a Bezel type — it exists only inside extern
//!   declarations
//! cites: 32.8
//! expect: reject
//! diagnostic: env
//! notes: valid twin drops the env position: `f: (int) -> int`.
module Conf;

public static class Apply
  public static method Run(f: (env, int) -> int): int
    return 0;
  end Run;
end Apply;

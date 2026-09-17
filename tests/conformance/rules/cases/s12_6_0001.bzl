//! case: s12_6_0001
//! doc: spec
//! section: 12.6
//! title: async abstract is rejected
//! rule: async abstract is a compile error. An abstract method whose
//!   implementations are expected to be asynchronous declares a Task or
//!   Task<T> return type without async.
//! cites: 12.6
//! expect: reject
//! diagnostic: async
module Conf;

public abstract class DataSource
  public constructor()
  end constructor;

  public async abstract method LoadAsync(): Task<string>;
end DataSource;

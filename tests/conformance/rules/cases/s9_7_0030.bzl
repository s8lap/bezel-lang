//! case: s9_7_0030
//! doc: spec
//! section: 9.7
//! title: a span is a value type that holds borrows and may not be an async parameter
//! rule: A value type that holds borrows is stored past its bound at no
//!   position: every position whose lifetime its carried borrows' sources do
//!   not provably bound — this bullet, the static-field bullet below, the
//!   collection bullet's escaping and wider-scoped collections, §30.1's frame
//!   rules, §30.5's `Task<T>` row, stdlib §6.5's `AsyncEventSource<T>` payload
//!   — is rejected for it statically
//! cites: 9.7, 30.1
//! expect: reject
//! diagnostic: async
//! issue: 5659
module Conf;

public class Client
  public async method Scan(s: ReadOnlySpan<int>): Task
  end Scan;
end Client;

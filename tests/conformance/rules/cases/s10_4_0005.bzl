//! case: s10_4_0005
//! doc: spec
//! section: 10.4
//! title: a user declaration cannot overload the compiler-provided FromNameOrNull
//! rule: Every enum carries three compiler-provided static members —
//!   `FromUnderlyingOrNull`, `FromNameOrNull`, `NameOf` — and no user
//!   declaration, a member or a declared constant, may override or overload
//!   any of them.
//! cites: 10.4
//! expect: reject
//! diagnostic: compiler-provided
module Conf;

enum Priority
  Low, Medium, High;

  public static method FromNameOrNull(name: string): Priority? => Low;
end Priority;

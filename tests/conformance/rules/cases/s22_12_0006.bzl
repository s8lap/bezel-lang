//! case: s22_12_0006
//! doc: spec
//! section: 22.12
//! title: static readonly of immutable content is unrestricted
//! rule: `static readonly` of immutable content — value types of
//!   sendable-value category, `string`, enums — and `const` are
//!   unrestricted (shared-immutable).
//! cites: 22.12
//! expect: accept
module Conf;

enum Level
  Low, Medium, High;
end Level;

public class Settings
  public static readonly Limit: int := 100;
  public static readonly Banner: string := "hello";
  public static readonly Default: Level := Level.Medium;
end Settings;

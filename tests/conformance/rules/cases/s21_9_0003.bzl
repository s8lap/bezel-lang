//! case: s21_9_0003
//! doc: spec
//! section: 21.9
//! title: a bare Task-family statement result is a hard error, not a warning
//! rule: For `Task`, the warning is replaced by a hard error directing the
//!   caller to the discard `_ := expression;`, `using var _ := expression;`,
//!   an explicit `.Dispose()`, or `Task.FireAndForget` as appropriate.
//! cites: 21.9, 17.13
//! expect: reject
//! diagnostic: FireAndForget
module Conf;

public class Program
  public static method Main()
    Task.FromResult(1);
  end Main;
end Program;

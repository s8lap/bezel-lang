//! case: s28_9_0003
//! doc: spec
//! section: 28.9
//! title: a function value carrying no captured borrow may be returned from its method
//! rule: Runtime management governs the function value itself, and the
//!   compiler tracks captured borrows separately from the function value's
//!   own lifetime — a function value that captures no borrow has an empty
//!   source set, so nothing it carries can be outlived and it is admitted
//!   at the escaping sinks a capture-bearing one is not.
//! cites: 28.9, 28.3
//! expect: accept
module Conf;

public class Program
  public method Make(): () -> ()
    return lambda() => Console.WriteLine("made");
  end Make;
end Program;

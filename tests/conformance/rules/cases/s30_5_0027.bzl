//! case: s30_5_0027
//! doc: spec
//! section: 30.5
//! title: Task<CancellationToken> is declarable at a parameter
//! rule: `CancellationToken` is excepted on §30.1's certified footing (axiom
//!   A10): `Task<CancellationToken>` is declarable, its result carrying §30.8's
//!   obligation that the source outlive every token that observes it
//! cites: 30.5, 30.8
//! expect: accept
//! notes: accepting twin of s30_5_0026.
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public record Pair(W: Widget);

public class Program
  public method Peek(t: Task<CancellationToken>): bool
    return t.IsCompleted;
  end Peek;
end Program;

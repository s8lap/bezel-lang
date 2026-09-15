//! case: s34_0001
//! doc: spec
//! section: 34
//! title: standard documentation tags on a type and its members are accepted
//! rule: Documentation comments use XML element syntax. Recognized tags
//!   include `<summary>`, `<param>`, `<returns>`, `<exception>`; the
//!   compiler preserves them in output metadata for documentation
//!   generators.
//! expect: accept
module Conf;

/// <summary>
/// Represents a labeled counter.
/// </summary>
public class Counter
  /// <summary>The current count.</summary>
  public property Value: int;

  /// <summary>
  /// Increments the count by the given amount.
  /// </summary>
  /// <param name="amount">The amount to add. Must be positive.</param>
  /// <exception cref="ArgumentException">Thrown when amount is non-positive.</exception>
  public method Increment(amount: int)
    if amount <= 0 then
      throw new ArgumentException("amount must be positive.", nameof(amount));
    end;
    Value := Value + amount;
  end Increment;
end Counter;

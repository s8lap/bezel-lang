//! case: s34_0005
//! doc: spec
//! section: 34
//! title: generic type parameters are documented with typemref and typemname tags
//! rule: Recognized tags include <typeparam>, <typeparamref>, and <paramref> —
//!   generic declarations are documented like any other.
//! expect: accept
//! cites: 34
module Conf;

/// <summary>
/// A pair of values.
/// </summary>
/// <typeparam name="TFirst">The type of the first value.</typeparam>
/// <typeparam name="TSecond">The type of the second value.</typeparam>
public class Pair<TFirst, TSecond>
  /// <summary>The first value.</summary>
  /// <typeparamref name="TFirst"/>
  public property First: TFirst;

  /// <summary>The second value.</summary>
  /// <typeparamref name="TSecond"/>
  public property Second: TSecond;
end Pair;

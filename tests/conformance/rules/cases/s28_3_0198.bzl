//! case: s28_3_0198
//! doc: spec
//! section: 28.3
//! title: a parameter typed Array<() -> T> exports a mint slot and is rejected at an iterable substitution
//! rule: A member whose declared type or signature carries a function type
//!   whose **return slot** (nullability stripped) is a bare type parameter
//!   anywhere other than as a plain, `owned` or `scoped` parameter's own
//!   declared function type ... — a return type (`Make(): () -> T`), ... or a
//!   **type argument** of any declared type, a parameter's included ... — is
//!   **inadmissible at an instantiation substituting a mint-announcing type**
//!   (`Task`-family or iterable-family) for that parameter, rejected at the
//!   instantiation
//! cites: 28.3, 3.6
//! expect: reject
//! diagnostic: mint
//! issue: 5904
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public interface IMaker<T>
  method Export(sink: Array<() -> T>);
end IMaker;

public class User
  public static method Run(m: IMaker<IEnumerable<int>>): int => 1;
end User;

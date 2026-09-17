//! case: l4_8_0010
//! doc: stdlib
//! section: 4.8
//! title: an auto-hashable record key is admitted at Dictionary<K,V>
//! rule: K must be hashable; an auto-hashable record — one whose storage
//!   is composed only of admissible component types — satisfies the
//!   constraint where the disqualified twin does not.
//! cites: 4.8, 3.6, 8.3
//! expect: accept
module Conf;

public record Key(Name: string);

public class Program
  public method Demo()
    using var d := new Dictionary<Key, int>();
  end Demo;
end Program;

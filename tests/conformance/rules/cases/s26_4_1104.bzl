//! case: s26_4_1104
//! doc: spec
//! section: 26.4
//! title: a generic extension on DrainedEntry<K, V> with where V: class is admitted
//! rule: by the parameters' constraints, which are the block's `where` clause and nothing else
//! cites: 14.2
//! expect: accept
//! issue: 5998
module Conf;

extension DrainedEntry<K, V> where V: class
  public method P(): int
    return 1;
  end P;
end;

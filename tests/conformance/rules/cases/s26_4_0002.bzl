//! case: s26_4_0002
//! doc: spec
//! section: 26.4
//! title: a nullable-marked identifier in an argument position is rejected
//! rule: A nullable-marked identifier in an argument position (`extension
//!   Array<T?>`) binds nothing and is a compile error naming this rule — only the
//!   bare non-nullable identifier introduces a parameter.
//! cites: 26.4
//! expect: reject
//! diagnostic: nullable
module Conf;

extension Array<T?>
  public method CountTwice(): int => this.Count * 2;
end;

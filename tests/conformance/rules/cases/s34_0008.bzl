//! case: s34_0008
//! doc: spec
//! section: 34
//! title: an empty documentation comment compiles
//! rule: Three comment forms (§1.5): `// ...` line comment, `/* ... */` block
//!   comment, `/// ...` documentation comment.
//! expect: accept
//! cites: 34
module Conf;

/// <summary/>

/// <param name="x"/>

/// <typeparam name="T"/>

public class DocEmpty
end DocEmpty;

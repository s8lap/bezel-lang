//! case: s22_3_0046
//! doc: spec
//! section: 22.3
//! title: a function type's ref slot of an owning choice is rejected at each written position
//! rule: A ref parameter whose declared type is a move-only value type
//!   (§15.3; an owning choice's nullable wrapper C? among them, §35.9), a
//!   function type's ref slot included, is a compile error at the
//!   declaration
//! cites: 22.3, 27.1
//! expect: reject
//! diagnostic: move-only value type
//! issue: 5986
module Conf;
class Res
    public constructor() end constructor;
end Res;
choice Bundle
    Ready(owned first: Res),
    Empty;
end Bundle;
class C
    private _f: ((ref Bundle) -> ())? := null;
    public static method G(): ((ref Bundle) -> ())?
        return null;
    end G;
    public static method Main(): int
        var f: ((ref Bundle) -> ())? := null;
        return 0;
    end Main;
end C;

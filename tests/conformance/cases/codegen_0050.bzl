var target: Stream := dest ?? _fallback;                       // both borrows, both sources outlive
using var x := (if useFresh then new Buffer() else src.CloneBuffer());   // both owned
using var y := (if a <> null then move a else move b);         // marked arms

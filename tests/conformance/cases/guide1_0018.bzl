using var x := (if useFresh then new Buffer() else source.CloneBuffer());
x.Process();
// Both arms are owned Buffer, so the expression has a single,
// unambiguous ownership: owned. `using var` discharges it at scope end.

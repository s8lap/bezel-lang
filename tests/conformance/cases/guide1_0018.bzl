using var x := (if useFresh then new Buffer() else source.CloneBuffer());
x.Process();
// CloneBuffer is declared `CloneBuffer(): owned Buffer`, so both arms are
// owned Buffer — the expression has a single, unambiguous ownership: owned.
// `using var` discharges it at scope end.

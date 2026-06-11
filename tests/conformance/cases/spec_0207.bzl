public method ProcessAndReplace()
  var fresh := new Stream();
  using var old := move _resource;        // moved into using var; disposed at scope end
  _resource := fresh;                      // restore
  // ... use old ...
  // at scope end: old.Dispose()
end ProcessAndReplace;

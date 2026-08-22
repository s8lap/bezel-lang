public method ProcessAndReplace()
  var fresh := new FileStream(_path, FileMode.Open);
  using var old := move _resource;        // moved into using var; disposed at scope end
  _resource := fresh;                      // restore
  // ... use old ...
  // at scope end: old.Dispose()
end ProcessAndReplace;

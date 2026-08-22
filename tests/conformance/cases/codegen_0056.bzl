public method Right(): owned FileStream
  var fresh := new FileStream(_path, FileMode.Open);   // build first — it may throw
  var old := move _resource;       // then take — nothing throwable follows
  _resource := fresh;              // restore
  return old;
end Right;

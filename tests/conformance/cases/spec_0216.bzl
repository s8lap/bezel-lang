public method Right(replace: bool): owned Stream
  var fresh := new FileStream(_path, FileMode.Open);
  var old := move _resource;
  _resource := fresh;
  return old;
end Right;

public method Right(replace: bool): owned Stream
  var fresh := new Stream();
  var old := move _resource;
  _resource := fresh;
  return old;
end Right;

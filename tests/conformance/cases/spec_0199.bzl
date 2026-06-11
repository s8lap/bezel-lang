public method ReplaceAndReturn(): owned Stream
  var fresh := new Stream();         // build replacement first
  var old := move _resource;          // take from field
  _resource := fresh;                  // restore field
  return old;                          // return the taken value
end ReplaceAndReturn;

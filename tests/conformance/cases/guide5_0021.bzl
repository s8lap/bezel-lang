var raw := LoadSamples();                             // owned OwningBuffer<double>
using var input := new Shared<FrozenArray<double>>(
    FrozenArray<double>.From(move raw));              // O(1): the storage is stolen

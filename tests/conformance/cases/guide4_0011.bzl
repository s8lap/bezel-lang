using var job := Parallel.Map(paths, ComputeSize);   // ComputeSize: static (string) -> int;
var sizes := await job;                              //   inputs chunk-copied at the call;
                                                     //   results in input order

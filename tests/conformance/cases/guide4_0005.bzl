var tasks := new Array<Task<string>>();
tasks.Add(File.ReadAllTextAsync(path));   // compiles — but WARNS (spec §15.4): the fresh
                                          //   task has no holder and is abandoned when
                                          //   the statement ends

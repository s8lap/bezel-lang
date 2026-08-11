_ := expression;            // value evaluated; if owned, disposed at END OF
                            //   STATEMENT (an unbound owned temporary). For a
                            //   value/runtime-managed type, dropped — the
                            //   discard only silences the unused-result
                            //   warning; a trivial-dispose class value is
                            //   freed at end of statement, no user code
                            //   running (§28.9). (Task/Task<T> excepted:
                            //   use Task.FireAndForget.)

using var _ := expression;  // value bound to an unnamed local that is
                            //   disposed at END OF SCOPE; works for any
                            //   class type.

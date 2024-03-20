# flexhq
The thread pool and epoll based socket server for rolexhound and smartwatch that implements netflex. Written up in https://www.youtube.com/watch?v=9jKqwEpilNE

Basic architecture is a thread pool worker-based listener where each worker has an epoll instance.
Accepted connections are added to workers until one fills up (max defined as a constant), and then added to the next one.

flexhq implements a publisher/subscriber model where rolexhound is a publisher and smartwatch is a subscriber (to filesystem events).

There is a subscriptions 'table' (an array of struct entries) where rolexhound will initialise itself with a NULL watchPath and non-NULL activeFiles.
Smartwatch will have all entries in the struct filled if it is a valid subscription entry. The table is reallocated in chunks until it reaches a maximum size.

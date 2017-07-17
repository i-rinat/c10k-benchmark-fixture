c10k reverse proxy fixture
--------------------------

Remember c10k? I was quite a problem back in early 2000s. But what about
hardware of today? Is it still as disastrous as it was back then?  Did you try
to implement HTTP reverse proxy in 10k threads and test?

So, here is implementation of a basic HTTP reverse proxy with thread per
connection. Ten thousands of threads, each waiting to pump data.  HTTP parser is
primitive. It understands version 1.0 only, and GET requests only. Should be
enough.

There is also epoll-based solution to implement. Not ready yet.

Target test was as follows. Enlarge number of open files to be able to handle at
least 20000 of them at a time. Prepare Nginx server, increase connection limit,
apply rate restriction. Say, 8192 bytes per second.  Prepare a file to
serve. Say, 163840 bytes. Backend should be slow to ensure all ten thousands
connections will be used at the same time, and proxy will be hopping from one
connection to another to pass another bit of data back to the client. Apache
Benchmark can be used as a client.  It makes connections as fast as possible, so
that's why slow backend is needed.

Results
-------

Nothing here. You should make measurements by yourself.

This file should contain:

  - Alexandra Koletsos
  - ak4749
  - Lab 7

The description should indicate whether your solution for the part is
working or not.  You may also want to include anything else you would
like to communicate to the grader such as extra functionalities you
implemented or how you tried to fix your non-working code.

Part 1:
Everything works as expected !!

valgrind --leak-check=yes ./mdb-lookup-server 5354 ~j-hui/cs3157-pub/bin/mdb-cs3157
==2196750== Memcheck, a memory error detector
==2196750== Copyright (C) 2002-2017, and GNU GPL'd, by Julian Seward et al.
==2196750== Using Valgrind-3.18.1 and LibVEX; rerun with -h for copyright info
==2196750== Command: ./mdb-lookup-server 5354 /home/j-hui/cs3157-pub/bin/mdb-cs3157
==2196750==
Connection started: 34.145.159.110
Connection terminated: 34.145.159.110
==2197781==
==2197781== HEAP SUMMARY:
==2197781==     in use at exit: 0 bytes in 0 blocks
==2197781==   total heap usage: 2,089 allocs, 2,089 frees, 76,160 bytes allocated
==2197781==
==2197781== All heap blocks were freed -- no leaks are possible
==2197781==
==2197781== For lists of detected and suppressed errors, rerun with: -s
==2197781== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
^C==2196750==
==2196750== Process terminating with default action of signal 2 (SIGINT)
==2196750==    at 0x498B5D7: accept (accept.c:26)
==2196750==    by 0x109973: main (mdb-lookup-server.c:153)
==2196750==
==2196750== HEAP SUMMARY:
==2196750==     in use at exit: 0 bytes in 0 blocks
==2196750==   total heap usage: 1 allocs, 1 frees, 64 bytes allocated
==2196750==
==2196750== All heap blocks were freed -- no leaks are possible
==2196750==
==2196750== For lists of detected and suppressed errors, rerun with: -s
==2196750== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

Part 2:
Everything works as expected. The memory leaks are constant (or at least I
hope they are).

valgrind --leak-check=yes ./http-server 4354 ~/html localhost 4356
==2211887== Memcheck, a memory error detector
==2211887== Copyright (C) 2002-2017, and GNU GPL'd, by Julian Seward et al.
==2211887== Using Valgrind-3.18.1 and LibVEX; rerun with -h for copyright info
==2211887== Command: ./http-server 4354 /mnt/disks/students229/ak4749/html localhost 4356
==2211887== 
connect: Connection refused
==2211887== 
==2211887== HEAP SUMMARY:
==2211887==     in use at exit: 64 bytes in 1 blocks
==2211887==   total heap usage: 3 allocs, 2 frees, 1,560 bytes allocated
==2211887== 
==2211887== LEAK SUMMARY:
==2211887==    definitely lost: 0 bytes in 0 blocks
==2211887==    indirectly lost: 0 bytes in 0 blocks
==2211887==      possibly lost: 0 bytes in 0 blocks
==2211887==    still reachable: 64 bytes in 1 blocks
==2211887==         suppressed: 0 bytes in 0 blocks
==2211887== Reachable blocks (those to which a pointer was found) are not shown.
==2211887== To see them, rerun with: --leak-check=full --show-leak-kinds=all
==2211887== 
==2211887== For lists of detected and suppressed errors, rerun with: -s
==2211887== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

I thought I felt pain before taking this class, but I guess I was wrong.

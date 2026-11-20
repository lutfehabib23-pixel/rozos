/* rozlibc — crti.o: TCC/gcc লিংকের সময় এটা চায় (.init/.fini-এর শুরু)। rozlibc
 * .init ব্যবহার করে না, তাই ফাঁকা। */
    .section .note.GNU-stack, "", @progbits

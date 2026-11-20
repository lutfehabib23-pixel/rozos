# rozOS menu entry — help menu
name: Compile and run hello.c
category: Development
run: set -e; tcc /src/hello.c -o /tmp/hello; /tmp/hello
about: tcc in one step
wait: yes

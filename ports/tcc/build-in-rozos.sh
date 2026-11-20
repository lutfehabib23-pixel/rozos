# rozOS-এর ভেতরে TCC দিয়ে TCC আবার বানানো (self-hosting-এর প্রথম ধাপ)
#   rozsh /usr/src/tcc/build-in-rozos.sh
# নতুনটা /tmp/tcc-new-এ হয়; পরীক্ষা করে নিজে /bin/tcc-তে কপি করুন: cp /tmp/tcc-new /bin/tcc
echo "building tcc with tcc ..."
tcc -o /tmp/tcc-new -I/usr/src/tcc /usr/src/tcc/tcc.c
echo "exit code: $?"
/tmp/tcc-new -v

sysroot/ = rozOS-এর "/" ফাইলসিস্টেমের বিষয়বস্তু।
make চালালে এখান থেকে build/initrd.tar তৈরি হয়, Limine সেটা মডিউল হিসেবে লোড করে,
আর কার্নেল বুটের সময় এটা RAM-এর tmpfs-এ ("/") খুলে দেয়।
খালি ডিরেক্টরি git-এ রাখতে ভেতরে ".keep" ফাইল আছে — initrd খোলার সময় সেগুলো বাদ যায়।

ধাপ ৫ থেকে: ext2 ডিস্ক (rozos-disk.img) থাকলে "/" সেখান থেকে আসে; `make disk` প্রথমবার
sysroot/ দিয়েই ডিস্ক বানায়। sysroot বদলালে `make disk-reset` (ডিস্কের সব ডেটা মোছে)।

ধাপ ৬ থেকে: userland/-এর প্রোগ্রামগুলো (init, hello, spin, crash) বিল্ড হয়ে
build/userland/bin-এ যায়, আর initrd ও ডিস্ক বানানোর সময় /bin-এ কপি হয় — তাই
sysroot/bin-এ এগুলো রাখতে হয় না। প্রোগ্রাম বদলালে `make run` নিজেই `make disk-update`
চালিয়ে ডিস্কের /bin-এ শুধু এগুলো বদলায় (debugfs দিয়ে) — ডিস্কের বাকি ফাইল অক্ষত থাকে।

ধাপ ৭ থেকে: userland/ এর প্রতিটি ফোল্ডারের (init, shell, editor, coreutils, bin, tests)
প্রতিটি .c একটা প্রোগ্রাম → /bin/<নাম> (debug-তথ্য ছাড়া কপি)। rozlibc-র হেডার
(libc/include) যায় /usr/include-এ, আর libc.a ও crt0.o যায় /usr/lib-এ — ধাপ ৮-এ rozOS-এর
ভেতরের TCC এগুলো দিয়েই প্রোগ্রাম বানাবে। home/.rozshrc = login shell-এর শুরুর স্ক্রিপ্ট।

ধাপ ৮ থেকে: TCC (./toolchain/get-tcc.sh দিয়ে আনা) → /bin/tcc, /usr/lib/tcc (libtcc1.a,
runmain.o, TCC-র নিজের হেডার), TCC-র সোর্স → /usr/src/tcc; crt1.o/crti.o/crtn.o/libm.a →
/usr/lib। docs/help/*.txt → /usr/share/help (shell-এ `help`)। প্রতিটি নতুন feature-এর সাথে
docs/help-এ পাতা যোগ/হালনাগাদ করতে হবে।

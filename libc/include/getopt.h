/* rozlibc — getopt.h (ধাপ ১৬ক): GNU getopt_long / getopt_long_only */
#ifndef _GETOPT_H
#define _GETOPT_H

extern char *optarg;
extern int optind, opterr, optopt;
extern int optreset;

struct option {
    const char *name;
    int has_arg;                 /* no_argument / required_argument / optional_argument */
    int *flag;                   /* NULL হলে val ফেরত, নইলে *flag = val, ফেরত 0 */
    int val;
};
#define no_argument       0
#define required_argument 1
#define optional_argument 2

int getopt(int argc, char *const argv[], const char *opts);
int getopt_long(int argc, char *const argv[], const char *opts,
                const struct option *longopts, int *longindex);
int getopt_long_only(int argc, char *const argv[], const char *opts,
                     const struct option *longopts, int *longindex);

#endif

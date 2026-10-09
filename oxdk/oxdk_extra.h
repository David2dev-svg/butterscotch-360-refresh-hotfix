#ifndef OXDK_EXTRA_H
#define OXDK_EXTRA_H
#include <sys/types.h>
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#endif

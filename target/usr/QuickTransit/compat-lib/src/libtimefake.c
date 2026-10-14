#include <sys/types.h>
#include <time.h>
#define faketime 1099774940
time_t time(time_t *loc)
{
        if (loc) *loc = faketime;
        return faketime;
}

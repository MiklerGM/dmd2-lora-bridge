#include <assert.h>
#include <stdio.h>

#include "../src/probe.h"

int main(void) {
    unsigned misses = 0;

    /* A healthy module that misses once or twice stays healthy; the third miss calls it gone. */
    assert(!probe_counts(&misses, false, true));
    assert(!probe_counts(&misses, false, true));
    assert(probe_counts(&misses, false, true));

    /* Once gone, silence is no news, and the first answer brings it back and clears the count. */
    assert(probe_counts(&misses, false, false));
    assert(probe_counts(&misses, true, false) && misses == 0);

    /* An answer in between starts the count again: two misses, an answer, two misses. */
    assert(!probe_counts(&misses, false, true));
    assert(!probe_counts(&misses, false, true));
    assert(probe_counts(&misses, true, true) && misses == 0);
    assert(!probe_counts(&misses, false, true));
    assert(!probe_counts(&misses, false, true));
    printf("%-28s ok\n", "probe misses");
    return 0;
}

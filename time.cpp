#include <utils/SystemClock.h>

extern "C" int64_t elapsed_realtime() {
    
    int64_t realtime = android::elapsedRealtime();
    
    return realtime;
}

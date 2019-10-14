
#include "globals.h"
#include "timers.h"
#include "clock.h"

  // initialize timer to this timepoint
void timer::reset() {
  t_start = t_total = 0.0;
  t_initial = gettime();
  count = 0;
}

double timer::start() {
  if (mynode() == 0) {
    t_start = gettime();
    return t_start;
  } else return 0.0;
}
  
double timer::end() {
  if (mynode() == 0) {
    double e = gettime();
    t_total += (e - t_start);
    count++;
    return e;
  } else return 0.0;
}

void timer::report(const char * label) {
  if (mynode() == 0) {
    static bool first = true;
    char line[100];

    if (first) {
      first = false;
      hila::output << "                       total(sec)          calls   usec/call  fraction\n";
    }
    // time used during the counter activity
    t_initial = gettime() - t_initial;
    if (count > 0) {
      std::snprintf(line,100," %16s: %14.3f %14llu %11.3f %8.4f\n",
                    label, t->total, t->count, 1e6 * t->total/t->count, t->total/t->initial );
    } else {
      std::snprintf(line,100," %16s: no timed calls made\n",label);
    }
    hila::output << line;      
  }
}
  
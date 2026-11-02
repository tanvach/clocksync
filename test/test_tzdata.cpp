// Checks tzdata.h against the host's zoneinfo database: for every zone the
// POSIX string must give the same UTC offset and DST flag as the full IANA
// rules, hour by hour, for the next few years. Run with `make -C test`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "timecode.h"
#include "tzdata.h"

struct Zone {
  std::string name, posix;
};

// Zones whose future offsets cannot be expressed as a POSIX rule. Morocco
// suspends DST during Ramadan, which tzdata lists as explicit transitions.
static const char *const kNotPosix[] = { "Africa/Casablanca", "Africa/El_Aaiun" };
// Zones whose negative transition times gen_tzdata.py clamps to 00:00; they
// may be off for an hour around each change.
static const char *const kApproximate[] = { "America/Nuuk", "America/Scoresbysund" };

static std::vector<Zone> parse_table() {
  std::vector<Zone> zones;
  const char *p = TZDATA;
  while (*p) {
    const char *tab = strchr(p, '\t');
    const char *nl = strchr(p, '\n');
    if (!tab || !nl || tab > nl) {
      printf("FAIL: malformed tzdata line near %.40s\n", p);
      exit(1);
    }
    zones.push_back({ std::string(p, tab - p), std::string(tab + 1, nl - tab - 1) });
    p = nl + 1;
  }
  return zones;
}

// tzdata version of the host, from macOS's +VERSION or Linux's tzdata.zi.
static std::string host_tzdata_version() {
  char buf[64] = "";
  if (FILE *f = fopen("/usr/share/zoneinfo/+VERSION", "r")) {
    if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
    fclose(f);
  } else if (FILE *f2 = fopen("/usr/share/zoneinfo/tzdata.zi", "r")) {
    char line[64];
    if (fgets(line, sizeof(line), f2) && sscanf(line, "# version %63s", buf) != 1) buf[0] = 0;
    fclose(f2);
  }
  std::string v(buf);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
  return v;
}

static void sample(const char *tz, time_t start, int hours, std::vector<long> &out) {
  setenv("TZ", tz, 1);
  tzset();
  out.clear();
  for (int h = 0; h < hours; h++) {
    time_t t = start + (time_t)h * 3600;
    struct tm lt;
    localtime_r(&t, &lt);
    out.push_back(lt.tm_gmtoff * 2 + (lt.tm_isdst > 0 ? 1 : 0));
  }
}

int main() {
  int failures = 0;
  std::vector<Zone> zones = parse_table();
  if ((int)zones.size() != TZDATA_COUNT) {
    printf("FAIL: TZDATA_COUNT %d but parsed %d zones\n", TZDATA_COUNT, (int)zones.size());
    failures++;
  }
  for (size_t i = 1; i < zones.size(); i++) {
    if (!(zones[i - 1].name < zones[i].name)) {
      printf("FAIL: zones not sorted/unique at %s\n", zones[i].name.c_str());
      failures++;
    }
  }

  // Each station's built-in zone must be in the table with the same rule.
  for (int s = 0; s < TC_STATION_COUNT; s++) {
    bool found = false;
    for (const Zone &z : zones) {
      if (z.name == tc_stations[s].native_tz) {
        found = true;
        if (z.posix != tc_stations[s].native_posix) {
          printf("FAIL: %s native_posix %s but tzdata has %s\n", tc_stations[s].key, tc_stations[s].native_posix,
                 z.posix.c_str());
          failures++;
        }
      }
    }
    if (!found) {
      printf("FAIL: %s native zone %s missing from tzdata\n", tc_stations[s].key, tc_stations[s].native_tz);
      failures++;
    }
  }

  // Rules differ between tzdata releases, so only compare like with like.
  std::string host = host_tzdata_version();
  if (host != TZDATA_VERSION) {
    printf("test_tzdata: skipped zone comparison, host tzdata %s but table is %s\n",
           host.empty() ? "unknown" : host.c_str(), TZDATA_VERSION);
    return failures ? 1 : 0;
  }

  // 2027-01-01 00:00 UTC, three years hourly. (The POSIX string describes
  // the rules after a zone's last listed change, e.g. Vancouver moved to
  // permanent UTC-7 on 2026-11-01.)
  const time_t start = 1798761600;
  const int hours = 3 * 365 * 24;
  std::vector<long> ref, got;
  int compared = 0, skipped = 0;
  for (const Zone &z : zones) {
    bool skip = false;
    for (const char *n : kNotPosix) skip |= z.name == n;
    if (skip) {
      skipped++;
      continue;
    }
    bool approximate = false;
    for (const char *n : kApproximate) approximate |= z.name == n;
    sample(z.name.c_str(), start, hours, ref);
    sample(z.posix.c_str(), start, hours, got);
    int bad = 0, first_bad = -1;
    for (int h = 0; h < hours; h++) {
      if (ref[h] != got[h]) {
        bad++;
        if (first_bad < 0) first_bad = h;
      }
    }
    // Approximate zones may be wrong for a few hours per change (6 changes).
    if (bad > (approximate ? 6 * 3 : 0)) {
      printf("FAIL: %s (%s) differs from zoneinfo for %d hours, first at %ld\n", z.name.c_str(), z.posix.c_str(),
             bad, (long)(start + (time_t)first_bad * 3600));
      failures++;
    }
    compared++;
  }
  printf("test_tzdata: %d zones compared, %d skipped, %d failures\n", compared, skipped, failures);
  return failures ? 1 : 0;
}

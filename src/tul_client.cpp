#include "tul_client.h"

#include "runtime_config.h"
#include "transit_cache.h"

#include <Arduino.h>
#include <FFat.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "miniz.h"

namespace {

constexpr char kGtfsZipPath[] = "/tul/pan.zip";
constexpr char kGtfsMetaPath[] = "/tul/meta.txt";
constexpr char kTulDir[] = "/tul";
constexpr uint32_t kGtfsRefreshSec = 86400u * 2u;
constexpr size_t kMaxDepartures = 64;
constexpr size_t kStreamChunk = 4096;
constexpr size_t kMaxActiveServices = 16;
constexpr size_t kMaxFilteredTrips = 4096;
constexpr size_t kLineBuf = 280;

struct GtfsRoute {
  char route_id[12];
  char short_name[12];
};

struct GtfsTrip {
  char trip_id[72];
  char route_id[12];
  char service_id[56];
};

void copy_cstr(char* dst, size_t dst_size, const char* src)
{
  if (dst_size == 0) {
    return;
  }
  if (src == nullptr) {
    src = "";
  }
  std::strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}

bool ffat_ready()
{
  if (FFat.begin(false)) {
    return true;
  }
  return FFat.begin(true);
}

bool ensure_tul_dir()
{
  if (!ffat_ready()) {
    return false;
  }
  if (!FFat.exists(kTulDir)) {
    return FFat.mkdir(kTulDir);
  }
  return true;
}

int yyyymmdd_local()
{
  const time_t now = time(nullptr);
  if (now <= 0) {
    return 0;
  }
  struct tm local_tm {};
  localtime_r(&now, &local_tm);
  return (local_tm.tm_year + 1900) * 10000 + (local_tm.tm_mon + 1) * 100 + local_tm.tm_mday;
}

int gtfs_wday_column(struct tm& local_tm)
{
  // GTFS calendar: monday=0 … sunday=6
  const int w = local_tm.tm_wday;
  return (w == 0) ? 6 : (w - 1);
}

bool str_ieq(const char* a, const char* b)
{
  if (a == nullptr || b == nullptr) {
    return a == b;
  }
  while (*a && *b) {
    if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool route_matches_line(const char* line_code, const GtfsRoute* routes, size_t route_count, const char* route_id)
{
  if (line_code == nullptr || line_code[0] == '\0') {
    return true;
  }
  if (route_id == nullptr) {
    return false;
  }
  if (str_ieq(route_id, line_code)) {
    return true;
  }
  for (size_t i = 0; i < route_count; ++i) {
    if (std::strcmp(routes[i].route_id, route_id) == 0 &&
        str_ieq(routes[i].short_name, line_code)) {
      return true;
    }
  }
  return false;
}

bool parse_hhmmss_to_minutes(const char* hhmmss, int& out_minutes)
{
  if (hhmmss == nullptr) {
    return false;
  }
  int h = 0;
  int m = 0;
  int s = 0;
  if (std::sscanf(hhmmss, "%d:%d:%d", &h, &m, &s) < 2) {
    return false;
  }
  out_minutes = h * 60 + m;
  return true;
}

int minutes_now_local()
{
  const time_t now = time(nullptr);
  if (now <= 0) {
    return -1;
  }
  struct tm local_tm {};
  localtime_r(&now, &local_tm);
  return local_tm.tm_hour * 60 + local_tm.tm_min;
}

void cache_path(const char* stop_id, const char* line_code, char* out, size_t out_size)
{
  char safe_stop[40] = "stop";
  char safe_line[16] = "all";
  if (stop_id != nullptr && stop_id[0] != '\0') {
    size_t j = 0;
    for (size_t i = 0; stop_id[i] != '\0' && j < sizeof(safe_stop) - 1; ++i) {
      const char c = stop_id[i];
      if (std::isalnum((unsigned char)c) || c == '_' || c == '-') {
        safe_stop[j++] = c;
      }
    }
    safe_stop[j] = '\0';
  }
  if (line_code != nullptr && line_code[0] != '\0') {
    size_t j = 0;
    for (size_t i = 0; line_code[i] != '\0' && j < sizeof(safe_line) - 1; ++i) {
      const char c = line_code[i];
      if (std::isalnum((unsigned char)c) || c == '_' || c == '-') {
        safe_line[j++] = c;
      }
    }
    safe_line[j] = '\0';
  }
  snprintf(out, out_size, "/tul/c_%s_%s.bin", safe_stop, safe_line);
}

bool read_cache(const char* path, int today, uint16_t* times, size_t& count)
{
  count = 0;
  if (!FFat.exists(path)) {
    return false;
  }
  File f = FFat.open(path, "r");
  if (!f) {
    return false;
  }
  char magic[4] = {};
  if (f.read((uint8_t*)magic, 4) != 4 || std::memcmp(magic, "TULC", 4) != 0) {
    f.close();
    return false;
  }
  int cached_day = 0;
  if (f.read((uint8_t*)&cached_day, sizeof(cached_day)) != sizeof(cached_day) || cached_day != today) {
    f.close();
    return false;
  }
  uint16_t n = 0;
  if (f.read((uint8_t*)&n, sizeof(n)) != sizeof(n) || n > kMaxDepartures) {
    f.close();
    return false;
  }
  for (uint16_t i = 0; i < n; ++i) {
    uint16_t t = 0;
    if (f.read((uint8_t*)&t, sizeof(t)) != sizeof(t)) {
      f.close();
      return false;
    }
    times[count++] = t;
  }
  f.close();
  return count > 0;
}

bool write_cache(const char* path, int today, const uint16_t* times, size_t count)
{
  File f = FFat.open(path, "w");
  if (!f) {
    return false;
  }
  const char magic[4] = {'T', 'U', 'L', 'C'};
  f.write((const uint8_t*)magic, 4);
  f.write((const uint8_t*)&today, sizeof(today));
  const uint16_t n = (uint16_t)count;
  f.write((const uint8_t*)&n, sizeof(n));
  for (size_t i = 0; i < count; ++i) {
    f.write((const uint8_t*)&times[i], sizeof(times[i]));
  }
  f.close();
  return true;
}

bool gtfs_zip_fresh()
{
  if (!FFat.exists(kGtfsZipPath) || !FFat.exists(kGtfsMetaPath)) {
    return false;
  }
  File meta = FFat.open(kGtfsMetaPath, "r");
  if (!meta) {
    return false;
  }
  char buf[24] = {};
  meta.readBytes(buf, sizeof(buf) - 1);
  meta.close();
  const uint32_t saved = (uint32_t)std::strtoul(buf, nullptr, 10);
  const uint32_t now = (uint32_t)time(nullptr);
  return saved > 0 && now > saved && (now - saved) < kGtfsRefreshSec;
}

bool download_gtfs_zip()
{
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (!ensure_tul_dir()) {
    return false;
  }

  HTTPClient http;
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  WiFiClientSecure tls;
  tls.setInsecure();
  if (!http.begin(tls, TUL_GTFS_ZIP_URL)) {
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    Serial.printf("[tul] GTFS download HTTP %d\n", code);
    return false;
  }
  const int len = http.getSize();
  File out = FFat.open(kGtfsZipPath, "w");
  if (!out) {
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int total = 0;
  while (http.connected() && (len < 0 || total < len)) {
    const size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      yield();
      continue;
    }
    const int rd = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (rd <= 0) {
      break;
    }
    out.write(buf, (size_t)rd);
    total += rd;
    yield();
  }
  out.close();
  http.end();

  File meta = FFat.open(kGtfsMetaPath, "w");
  if (meta) {
    meta.printf("%lu", (unsigned long)time(nullptr));
    meta.close();
  }
  Serial.printf("[tul] GTFS zip saved (%d bytes)\n", total);
  transit_cache_invalidate_provider(kTransitProviderTul);
  return total > 1000;
}

bool zip_read_member_to_string(mz_zip_archive& zip, const char* filename, String& out)
{
  const int idx = mz_zip_reader_locate_file(&zip, filename, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (idx < 0) {
    return false;
  }
  size_t uncomp_size = 0;
  void* p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &uncomp_size, 0);
  if (p == nullptr || uncomp_size == 0) {
    return false;
  }
  out.reserve(uncomp_size + 1);
  out = "";
  out.concat((const char*)p, uncomp_size);
  mz_free(p);
  return true;
}

bool parse_routes(const String& text, GtfsRoute* routes, size_t& route_count)
{
  route_count = 0;
  int line_start = 0;
  bool header = true;
  int col_route_id = 0;
  int col_short = 2;
  while (line_start < (int)text.length()) {
    int line_end = text.indexOf('\n', line_start);
    if (line_end < 0) {
      line_end = text.length();
    }
    String line = text.substring(line_start, line_end);
    line_start = line_end + 1;
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    if (header) {
      header = false;
      col_route_id = 0;
      col_short = 2;
      int col = 0;
      int field_start = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          const String field = line.substring(field_start, i);
          if (field == "route_id") {
            col_route_id = col;
          } else if (field == "route_short_name") {
            col_short = col;
          }
          ++col;
          field_start = i + 1;
        }
      }
      continue;
    }
    if (route_count >= 24) {
      break;
    }
    int col = 0;
    int field_start = 0;
    GtfsRoute r{};
    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        const String field = line.substring(field_start, i);
        if (col == col_route_id) {
          copy_cstr(r.route_id, sizeof(r.route_id), field.c_str());
        } else if (col == col_short) {
          copy_cstr(r.short_name, sizeof(r.short_name), field.c_str());
        }
        ++col;
        field_start = i + 1;
      }
    }
    if (r.route_id[0] != '\0') {
      routes[route_count++] = r;
    }
  }
  return route_count > 0;
}

struct ActiveServices {
  char ids[kMaxActiveServices][56];
  size_t count = 0;
};

bool service_id_active(const ActiveServices& active, const char* service_id)
{
  if (service_id == nullptr || service_id[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < active.count; ++i) {
    if (std::strcmp(active.ids[i], service_id) == 0) {
      return true;
    }
  }
  return false;
}

bool build_active_services(const String& calendar_text, int today, int wday_col, ActiveServices& out)
{
  out.count = 0;
  int line_start = 0;
  bool header = true;
  int col_service = 0;
  int col_wday = 1;
  int col_start = 8;
  int col_end = 9;
  while (line_start < (int)calendar_text.length()) {
    int line_end = calendar_text.indexOf('\n', line_start);
    if (line_end < 0) {
      line_end = calendar_text.length();
    }
    String line = calendar_text.substring(line_start, line_end);
    line_start = line_end + 1;
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    if (header) {
      header = false;
      int col = 0;
      int field_start = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          const String field = line.substring(field_start, i);
          if (field == "service_id") {
            col_service = col;
          } else if (field == "monday") {
            col_wday = col;
          } else if (field == "start_date") {
            col_start = col;
          } else if (field == "end_date") {
            col_end = col;
          }
          ++col;
          field_start = i + 1;
        }
      }
      continue;
    }
    int col = 0;
    int field_start = 0;
    char sid[56] = {};
    int active = 0;
    int start_date = 0;
    int end_date = 0;
    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        const String field = line.substring(field_start, i);
        if (col == col_service) {
          copy_cstr(sid, sizeof(sid), field.c_str());
        } else if (col == col_wday + wday_col) {
          active = field.toInt();
        } else if (col == col_start) {
          start_date = field.toInt();
        } else if (col == col_end) {
          end_date = field.toInt();
        }
        ++col;
        field_start = i + 1;
      }
    }
    if (active != 0 && today >= start_date && today <= end_date && sid[0] != '\0' &&
        out.count < kMaxActiveServices) {
      copy_cstr(out.ids[out.count++], 56, sid);
    }
  }
  return out.count > 0;
}

int trip_cmp(const void* a, const void* b)
{
  return std::strcmp(((const GtfsTrip*)a)->trip_id, ((const GtfsTrip*)b)->trip_id);
}

GtfsTrip* load_filtered_trips(const String& text,
                              const ActiveServices& active,
                              const char* line_code,
                              const GtfsRoute* routes,
                              size_t route_count,
                              size_t& trip_count)
{
  trip_count = 0;
  GtfsTrip* trips = (GtfsTrip*)heap_caps_malloc(kMaxFilteredTrips * sizeof(GtfsTrip),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (trips == nullptr) {
    trips = (GtfsTrip*)malloc(kMaxFilteredTrips * sizeof(GtfsTrip));
  }
  if (trips == nullptr) {
    return nullptr;
  }

  int line_start = 0;
  bool header = true;
  int col_route = 0;
  int col_service = 1;
  int col_trip = 2;
  while (line_start < (int)text.length() && trip_count < kMaxFilteredTrips) {
    int line_end = text.indexOf('\n', line_start);
    if (line_end < 0) {
      line_end = text.length();
    }
    String line = text.substring(line_start, line_end);
    line_start = line_end + 1;
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    if (header) {
      header = false;
      int col = 0;
      int field_start = 0;
      for (int i = 0; i <= line.length(); ++i) {
        if (i == line.length() || line[i] == ',') {
          const String field = line.substring(field_start, i);
          if (field == "route_id") {
            col_route = col;
          } else if (field == "service_id") {
            col_service = col;
          } else if (field == "trip_id") {
            col_trip = col;
          }
          ++col;
          field_start = i + 1;
        }
      }
      continue;
    }
    int col = 0;
    int field_start = 0;
    GtfsTrip t{};
    for (int i = 0; i <= line.length(); ++i) {
      if (i == line.length() || line[i] == ',') {
        const String field = line.substring(field_start, i);
        if (col == col_route) {
          copy_cstr(t.route_id, sizeof(t.route_id), field.c_str());
        } else if (col == col_service) {
          copy_cstr(t.service_id, sizeof(t.service_id), field.c_str());
        } else if (col == col_trip) {
          copy_cstr(t.trip_id, sizeof(t.trip_id), field.c_str());
        }
        ++col;
        field_start = i + 1;
      }
    }
    if (t.trip_id[0] == '\0' || !service_id_active(active, t.service_id) ||
        !route_matches_line(line_code, routes, route_count, t.route_id)) {
      continue;
    }
    trips[trip_count++] = t;
  }

  if (trip_count > 1) {
    qsort(trips, trip_count, sizeof(GtfsTrip), trip_cmp);
  }
  return trips;
}

const GtfsTrip* find_trip_bsearch(const GtfsTrip* trips, size_t trip_count, const char* trip_id)
{
  if (trips == nullptr || trip_id == nullptr || trip_count == 0) {
    return nullptr;
  }
  size_t lo = 0;
  size_t hi = trip_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = std::strcmp(trips[mid].trip_id, trip_id);
    if (cmp == 0) {
      return &trips[mid];
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

void insert_sorted_unique(uint16_t* times, size_t& count, uint16_t value)
{
  for (size_t i = 0; i < count; ++i) {
    if (times[i] == value) {
      return;
    }
  }
  if (count >= kMaxDepartures) {
    return;
  }
  times[count++] = value;
}

bool parse_csv_field(const char* line, int field_index, char* out, size_t out_size)
{
  if (line == nullptr || out_size == 0) {
    return false;
  }
  int col = 0;
  const char* start = line;
  for (const char* p = line; ; ++p) {
    if (*p == ',' || *p == '\0') {
      if (col == field_index) {
        size_t len = (size_t)(p - start);
        if (len >= out_size) {
          len = out_size - 1;
        }
        std::memcpy(out, start, len);
        out[len] = '\0';
        return len > 0;
      }
      if (*p == '\0') {
        break;
      }
      ++col;
      start = p + 1;
    }
  }
  return false;
}

void process_stop_time_line(const char* line,
                            const char* stop_id,
                            const GtfsTrip* trips,
                            size_t trip_count,
                            uint16_t* times,
                            size_t& count)
{
  if (line == nullptr || line[0] == '\0') {
    return;
  }
  char trip_id[72] = {};
  char dep_time[16] = {};
  char stop_val[40] = {};
  if (!parse_csv_field(line, 0, trip_id, sizeof(trip_id)) ||
      !parse_csv_field(line, 2, dep_time, sizeof(dep_time)) ||
      !parse_csv_field(line, 3, stop_val, sizeof(stop_val))) {
    return;
  }
  if (std::strcmp(stop_val, stop_id) != 0) {
    return;
  }
  if (find_trip_bsearch(trips, trip_count, trip_id) == nullptr) {
    return;
  }
  int dep_min = 0;
  if (!parse_hhmmss_to_minutes(dep_time, dep_min)) {
    return;
  }
  insert_sorted_unique(times, count, (uint16_t)dep_min);
}

bool parse_stop_times_stream(mz_zip_archive& zip,
                             const char* stop_id,
                             const GtfsTrip* trips,
                             size_t trip_count,
                             uint16_t* times,
                             size_t& count)
{
  count = 0;
  const int idx = mz_zip_reader_locate_file(&zip, "stop_times.txt", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (idx < 0) {
    return false;
  }

  char stop_pat[48];
  snprintf(stop_pat, sizeof(stop_pat), ",%s,", stop_id);

  mz_zip_reader_extract_iter_state* iter =
      mz_zip_reader_extract_iter_new(&zip, (mz_uint)idx, 0);
  if (iter == nullptr) {
    return false;
  }

  char carry[kLineBuf * 2] = {};
  size_t carry_len = 0;
  bool header_done = false;
  uint8_t buf[kStreamChunk];
  while (true) {
    const size_t rd = mz_zip_reader_extract_iter_read(iter, buf, sizeof(buf));
    if (rd == 0) {
      break;
    }
    size_t off = 0;
    while (off < rd) {
      const char c = (char)buf[off++];
      if (c != '\n') {
        if (carry_len + 1 < sizeof(carry)) {
          carry[carry_len++] = c;
        }
        continue;
      }
      carry[carry_len] = '\0';
      carry_len = 0;
      if (!header_done) {
        header_done = true;
        continue;
      }
      if (carry[0] == '\0' || std::strstr(carry, stop_pat) == nullptr) {
        continue;
      }
      process_stop_time_line(carry, stop_id, trips, trip_count, times, count);
    }
    yield();
  }
  if (carry_len > 0 && header_done) {
    carry[carry_len] = '\0';
    if (std::strstr(carry, stop_pat) != nullptr) {
      process_stop_time_line(carry, stop_id, trips, trip_count, times, count);
    }
  }
  mz_zip_reader_extract_iter_free(iter);
  return count > 0;
}

bool pick_next_departure(const uint16_t* times, size_t count, int now_min, int& best_min, int& best_dep_min)
{
  best_min = 9999;
  best_dep_min = -1;
  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    int dep = times[i];
    int delta = dep - now_min;
    if (delta < 0 && dep < 24 * 60) {
      continue;
    }
    if (delta < 0) {
      delta += 24 * 60;
    }
    if (delta < best_min) {
      best_min = delta;
      best_dep_min = dep % (24 * 60);
      found = true;
    }
  }
  if (!found && count > 0) {
    best_dep_min = times[0] % (24 * 60);
    best_min = (best_dep_min - now_min + 24 * 60) % (24 * 60);
    found = true;
  }
  return found;
}

bool build_departures(const char* stop_id,
                      const char* line_code,
                      uint16_t* times,
                      size_t& count)
{
  count = 0;
  if (!ensure_tul_dir()) {
    return false;
  }
  if (!gtfs_zip_fresh()) {
    if (!download_gtfs_zip()) {
      return false;
    }
  }

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, kGtfsZipPath, 0)) {
    Serial.println(F("[tul] zip open failed"));
    return false;
  }

  String routes_txt;
  String calendar_txt;
  String trips_txt;
  if (!zip_read_member_to_string(zip, "routes.txt", routes_txt) ||
      !zip_read_member_to_string(zip, "calendar.txt", calendar_txt) ||
      !zip_read_member_to_string(zip, "trips.txt", trips_txt)) {
    mz_zip_reader_end(&zip);
    return false;
  }

  GtfsRoute routes[24];
  size_t route_count = 0;
  parse_routes(routes_txt, routes, route_count);

  const int today = yyyymmdd_local();
  struct tm local_tm {};
  const time_t now = time(nullptr);
  localtime_r(&now, &local_tm);
  const int wday_col = gtfs_wday_column(local_tm);

  ActiveServices active{};
  if (!build_active_services(calendar_txt, today, wday_col, active)) {
    mz_zip_reader_end(&zip);
    return false;
  }

  size_t trip_count = 0;
  GtfsTrip* trips = load_filtered_trips(trips_txt, active, line_code, routes, route_count, trip_count);
  if (trips == nullptr || trip_count == 0) {
    mz_zip_reader_end(&zip);
    return false;
  }
  Serial.printf("[tul] trips filtered=%u\n", (unsigned)trip_count);

  const bool ok = parse_stop_times_stream(zip, stop_id, trips, trip_count, times, count);

  free(trips);
  mz_zip_reader_end(&zip);
  return ok && count > 0;
}

} // namespace

bool tul_fetch_next_departure(const char* api_key,
                              const char* api_base,
                              const char* stop_id,
                              const char* line_code,
                              const char* feed_key,
                              const char* fallback_line_label,
                              bool prefer_theoretical,
                              IdfmResult& result)
{
  (void)api_key;
  (void)api_base;
  (void)feed_key;

  result.ok = false;
  result.minutes = -1;
  result.disrupted = false;
  copy_cstr(result.error, sizeof(result.error), "tul");
  copy_cstr(result.text, sizeof(result.text), "--");

  if (stop_id == nullptr || stop_id[0] == '\0') {
    copy_cstr(result.error, sizeof(result.error), "no stop");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    copy_cstr(result.error, sizeof(result.error), "no wifi");
    return false;
  }

  const int today = yyyymmdd_local();
  const int now_min = minutes_now_local();
  if (today <= 0 || now_min < 0) {
    copy_cstr(result.error, sizeof(result.error), "no time");
    return false;
  }

  char cache_file[72];
  cache_path(stop_id, line_code, cache_file, sizeof(cache_file));

  uint16_t times[kMaxDepartures];
  size_t count = 0;
  if (!read_cache(cache_file, today, times, count)) {
    Serial.printf("[tul] building cache %s\n", cache_file);
    if (!build_departures(stop_id, line_code, times, count)) {
      copy_cstr(result.error, sizeof(result.error), "gtfs");
      return false;
    }
    write_cache(cache_file, today, times, count);
  }

  int best_delta = 0;
  int best_dep = 0;
  if (!pick_next_departure(times, count, now_min, best_delta, best_dep)) {
    copy_cstr(result.error, sizeof(result.error), "no match");
    return false;
  }

  if (fallback_line_label != nullptr && fallback_line_label[0] != '\0') {
    copy_cstr(result.line, sizeof(result.line), fallback_line_label);
  } else if (line_code != nullptr && line_code[0] != '\0') {
    copy_cstr(result.line, sizeof(result.line), line_code);
  }

  result.minutes = best_delta > 0 ? best_delta - 1 : 0;
  if (prefer_theoretical) {
    snprintf(result.text, sizeof(result.text), "%02d:%02d", best_dep / 60, best_dep % 60);
  } else {
    cfg_format_bus_eta_text(result.minutes, false, nullptr, result.text, sizeof(result.text));
  }
  result.ok = true;
  copy_cstr(result.error, sizeof(result.error), "");
  return true;
}

bool tul_line_has_disruption(const char* api_key,
                             const char* api_base,
                             const char* stop_id,
                             const char* feed_key,
                             bool& out_disrupted)
{
  (void)api_key;
  (void)api_base;
  (void)stop_id;
  (void)feed_key;
  out_disrupted = false;
  return true;
}

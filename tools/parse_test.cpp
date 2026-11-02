// Host-side test of the firmware JSON scanning helpers against real ComfyUI responses.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
using std::string;

static const char *sp(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}
static const char *keyVal(const char *s, const char *key) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(s, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  p = sp(p);
  if (*p != ':') return nullptr;
  p = sp(p + 1);
  return p;
}
static double jnum(const char *s, const char *key, double def = 0) {
  const char *v = keyVal(s, key);
  return v ? strtod(v, nullptr) : def;
}
static void jstr(char *out, size_t n, const char *s, const char *key) {
  out[0] = 0;
  const char *v = keyVal(s, key);
  if (!v || *v != '"') return;
  v++;
  size_t i = 0;
  while (v[i] && i < n - 1) {
    if (v[i] == '\\' && (v[i + 1] == '"' || v[i + 1] == '\\')) { out[i] = v[i + 1]; i++; v += 2; continue; }
    if (v[i] == '"') break;
    out[i] = v[i];
    i++;
  }
  out[i] = 0;
}
static const char *findIn(const char *hay, size_t len, const char *needle, size_t from) {
  if (from >= len) return nullptr;
  size_t nl = strlen(needle);
  if (nl > len - from) return nullptr;
  for (size_t i = from; i + nl <= len; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0) return hay + i;
  return nullptr;
}
static double numIn(const char *base, size_t len, size_t from, const char *key) {
  const char *k = findIn(base, len, key, from);
  if (!k) return 0;
  const char *v = sp(k + strlen(key));
  if (*v != ':') return 0;
  v = sp(v + 1);
  return strtod(v, nullptr);
}
static int countArrElems(const char *base, size_t len, const char *key) {
  const char *k = findIn(base, len, key, 0);
  if (!k) return -1;
  const char *p = sp(k + strlen(key));
  if (*p != ':') return -1;
  p = sp(p + 1);
  if (*p != '[') return -1;
  int depth = 0, cnt = 0;
  bool instr = false, esc = false;
  for (; *p; p++) {
    char c = *p;
    if (instr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') instr = false; continue; }
    if (c == '"') { instr = true; continue; }
    if (c == '[' || c == '{') { depth++; if (depth == 2) cnt++; }
    else if (c == ']' || c == '}') { depth--; if (depth == 0) break; }
  }
  return cnt;
}

struct Job { char file[56]; uint32_t dur_ms; uint32_t end_epoch; bool ok; };

static int parseHistory(const char *s, size_t len, Job *jobs, int maxJobs) {
  size_t starts[8];
  int n = 0;
  size_t p = 0;
  const char *needle = "\"prompt\":";
  while (n < 8) {
    const char *q = findIn(s, len, needle, p);
    if (!q) break;
    size_t off = q - s + strlen(needle);
    const char *c = sp(q + strlen(needle));
    if (*c == '[') { starts[n++] = q - s; p = off; }
    else p = off;
  }
  if (n == 0) return 0;
  Job tmp[8];
  int tn = 0;
  for (int i = 0; i < n && tn < maxJobs; i++) {
    size_t a = starts[i];
    size_t e = (i + 1 < n) ? starts[i + 1] : len;
    Job &j = tmp[tn];
    memset(&j, 0, sizeof(j));
    const char *fn = findIn(s, e, "\"filename\"", a);
    if (fn) {
      const char *v = sp(fn + strlen("\"filename\""));
      if (*v == ':') {
        v = sp(v + 1);
        if (*v == '"') {
          v++;
          size_t k = 0;
          while (v[k] && v[k] != '"' && k < sizeof(j.file) - 1) { j.file[k] = v[k]; k++; }
          j.file[k] = 0;
        }
      }
    }
    const char *ss = findIn(s, e, "\"status_str\"", a);
    j.ok = true;
    if (ss) {
      const char *v = sp(sp(ss + 12) + 1);
      j.ok = strncmp(v, "\"success\"", 9) == 0;
    }
    if (!j.file[0] && j.ok) continue;
    if (!j.file[0]) strcpy(j.file, "(failed)");
    const char *es = findIn(s, e, "\"execution_start\"", a);
    const char *su = findIn(s, e, "\"execution_success\"", a);
    if (!su) su = findIn(s, e, "\"execution_error\"", a);
    double start_ts = es ? numIn(s, e, (size_t)(es - s), "\"timestamp\"") : 0;
    double succ_ts = su ? numIn(s, e, (size_t)(su - s), "\"timestamp\"") : 0;
    j.dur_ms = (succ_ts > start_ts) ? (uint32_t)(succ_ts - start_ts) : 0;
    j.end_epoch = (uint32_t)(succ_ts / 1000);
    tmp[tn++] = j;
  }
  for (int i = 0; i < tn; i++) jobs[i] = tmp[tn - 1 - i];
  return tn;
}

static string slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  string s; s.resize(n);
  if (fread(&s[0], 1, n, f) != (size_t)n) { perror("read"); exit(1); }
  fclose(f);
  return s;
}

int main(int argc, char **argv) {
  string stats = slurp(argc > 1 ? argv[1] : "stats.json");
  string queue = slurp(argc > 2 ? argv[2] : "queue.json");
  string hist = slurp(argc > 3 ? argv[3] : "history.json");

  printf("== stats\n");
  printf("ram_total=%.0f ram_free=%.0f vram_total=%.0f vram_free=%.0f\n",
         jnum(stats.c_str(), "ram_total"), jnum(stats.c_str(), "ram_free"),
         jnum(stats.c_str(), "vram_total"), jnum(stats.c_str(), "vram_free"));
  char name[40], ver[12];
  const char *dev = strstr(stats.c_str(), "\"devices\"");
  jstr(name, sizeof(name), dev ? dev : stats.c_str(), "name");
  jstr(ver, sizeof(ver), stats.c_str(), "comfyui_version");
  printf("gpu='%s' ver='%s'\n", name, ver);
  double tu = jnum(stats.c_str(), "torch_vram_total") - jnum(stats.c_str(), "torch_vram_free");
  printf("torch_used=%.0f (%.2f GB)\n", tu, tu / 1073741824.0);

  printf("== queue (len=%zu) running=%d pending=%d\n", queue.size(),
         countArrElems(queue.c_str(), queue.size(), "\"queue_running\""),
         countArrElems(queue.c_str(), queue.size(), "\"queue_pending\""));

  printf("== history (len=%zu)\n", hist.size());
  Job jobs[6];
  int n = parseHistory(hist.c_str(), hist.size(), jobs, 6);
  for (int i = 0; i < n; i++)
    printf("  [%d] ok=%d file=%s dur=%.1fs end=%u\n", i, jobs[i].ok, jobs[i].file,
           jobs[i].dur_ms / 1000.0, jobs[i].end_epoch);
  return 0;
}

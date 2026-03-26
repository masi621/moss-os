#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/moss-bridge.sock"
#define DEFAULT_DB     "/var/lib/moss/moss.db"
#define DEFAULT_PID    "/run/moss-bridge.pid"
#define BUF_SIZE       65536
#define MAX_CLIENTS    32
#define POLL_INTERVAL_MS 1000

#ifndef SQLITE_TRANSIENT
#define SQLITE_TRANSIENT ((void(*)(void*))-1)
#endif

typedef struct {
    pthread_mutex_t lock;
    char hostname[64];
    char kernel[128];
    char cpu_model[128];
    int cpu_pct;
    int cpu_temp_c;
    int cpu_freq_mhz;
    uint64_t mem_total_kb;
    uint64_t mem_used_kb;
    int mem_pct;
    uint64_t disk_total_mb;
    uint64_t disk_used_mb;
    int disk_pct;
    uint64_t uptime_s;
} MetricCache;

static volatile sig_atomic_t g_running = 1;
static char g_socket_path[256] = DEFAULT_SOCKET;
static char g_db_path[256] = DEFAULT_DB;
static char g_pid_path[256] = DEFAULT_PID;
static int g_server_fd = -1;
static sqlite3 *g_db = NULL;
static MetricCache g_metrics = { .lock = PTHREAD_MUTEX_INITIALIZER };

typedef struct { uint64_t total; uint64_t idle; } CpuTimes;
static CpuTimes g_prev_cpu = {0, 0};

static int appendf(char *dst, int pos, int cap, const char *fmt, ...) {
    va_list ap;
    int wrote;
    if (pos >= cap) return pos;
    va_start(ap, fmt);
    wrote = vsnprintf(dst + pos, (size_t)(cap - pos), fmt, ap);
    va_end(ap);
    if (wrote < 0) return pos;
    if (wrote >= cap - pos) return cap;
    return pos + wrote;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t cap = 4096, len = 0;
    int ch;
    if (!f) return NULL;
    buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    while ((ch = fgetc(f)) != EOF) {
        if (len + 2 >= cap) {
            char *tmp;
            cap *= 2;
            tmp = realloc(buf, cap);
            if (!tmp) { free(buf); fclose(f); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void ensure_parent_dir(const char *path) {
    char tmp[512];
    char *slash;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';
    if (!*tmp) return;
    mkdir(tmp, 0755);
}

static int json_get_str(const char *json, const char *key, char *out, size_t out_sz) {
    char needle[128];
    const char *p;
    size_t o = 0;
    if (!json || !key || !out || out_sz == 0) return 0;
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && *p != '"' && o + 1 < out_sz) {
        if (*p == '\\' && p[1]) ++p;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 1;
}

static long long json_get_int(const char *json, const char *key, long long def) {
    char needle[128];
    const char *p;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') ++p;
    return atoll(p);
}

static void json_escape(const char *src, char *dst, size_t dst_sz) {
    size_t o = 0;
    while (*src && o + 2 < dst_sz) {
        switch (*src) {
            case '\\': dst[o++]='\\'; dst[o++]='\\'; break;
            case '"':  dst[o++]='\\'; dst[o++]='"'; break;
            case '\n': dst[o++]='\\'; dst[o++]='n'; break;
            case '\r': dst[o++]='\\'; dst[o++]='r'; break;
            case '\t': dst[o++]='\\'; dst[o++]='t'; break;
            default:   dst[o++]=*src; break;
        }
        ++src;
    }
    dst[o] = '\0';
}

static int poll_cpu_pct(void) {
    char *raw = read_file("/proc/stat");
    uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, soft = 0, steal = 0;
    uint64_t total, idle_total, dt, di;
    int pct = 0;
    if (!raw) return 0;
    sscanf(raw, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iowait, &irq, &soft, &steal);
    free(raw);
    total = user + nice + sys + idle + iowait + irq + soft + steal;
    idle_total = idle + iowait;
    if (g_prev_cpu.total != 0) {
        dt = total - g_prev_cpu.total;
        di = idle_total - g_prev_cpu.idle;
        if (dt) pct = (int)(((dt - di) * 100ULL) / dt);
    }
    g_prev_cpu.total = total;
    g_prev_cpu.idle = idle_total;
    return pct;
}

static int poll_cpu_temp(void) {
    static const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/hwmon/hwmon0/temp1_input",
        "/sys/class/hwmon/hwmon1/temp1_input",
        NULL
    };
    int i;
    for (i = 0; paths[i]; ++i) {
        FILE *f = fopen(paths[i], "r");
        if (f) {
            int temp = 0;
            if (fscanf(f, "%d", &temp) == 1) {
                fclose(f);
                return temp / 1000;
            }
            fclose(f);
        }
    }
    return -1;
}

static int poll_cpu_freq(void) {
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    int freq = 0;
    if (!f) return -1;
    if (fscanf(f, "%d", &freq) != 1) freq = -1;
    fclose(f);
    return freq < 0 ? -1 : freq / 1000;
}

static void poll_memory(MetricCache *m) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    uint64_t total = 0, avail = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) total = (uint64_t)v;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) avail = (uint64_t)v;
    }
    fclose(f);
    m->mem_total_kb = total;
    m->mem_used_kb = (avail <= total) ? (total - avail) : 0;
    m->mem_pct = total ? (int)((m->mem_used_kb * 100ULL) / total) : 0;
}

static void poll_disk(MetricCache *m) {
    struct statvfs st;
    if (statvfs("/", &st) == 0 && st.f_frsize) {
        uint64_t total = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
        uint64_t avail = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
        uint64_t used = total >= avail ? total - avail : 0;
        m->disk_total_mb = total / (1024ULL * 1024ULL);
        m->disk_used_mb = used / (1024ULL * 1024ULL);
        m->disk_pct = total ? (int)((used * 100ULL) / total) : 0;
    }
}

static void poll_system(MetricCache *m) {
    FILE *f;
    char line[256];

    f = fopen("/proc/sys/kernel/hostname", "r");
    if (f) {
        if (fgets(m->hostname, sizeof(m->hostname), f)) m->hostname[strcspn(m->hostname, "\n")] = '\0';
        fclose(f);
    }

    f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(m->kernel, sizeof(m->kernel), f)) m->kernel[strcspn(m->kernel, "\n")] = '\0';
        fclose(f);
    }

    f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0.0;
        if (fscanf(f, "%lf", &up) == 1) m->uptime_s = (uint64_t)up;
        fclose(f);
    }

    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    ++colon;
                    while (*colon == ' ') ++colon;
                    strncpy(m->cpu_model, colon, sizeof(m->cpu_model) - 1);
                    m->cpu_model[sizeof(m->cpu_model) - 1] = '\0';
                    m->cpu_model[strcspn(m->cpu_model, "\n")] = '\0';
                    break;
                }
            }
        }
        fclose(f);
    }
}

static void *poll_thread(void *arg) {
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_metrics.lock);
        poll_system(&g_metrics);
        g_metrics.cpu_pct = poll_cpu_pct();
        g_metrics.cpu_temp_c = poll_cpu_temp();
        g_metrics.cpu_freq_mhz = poll_cpu_freq();
        poll_memory(&g_metrics);
        poll_disk(&g_metrics);
        pthread_mutex_unlock(&g_metrics.lock);
        usleep(POLL_INTERVAL_MS * 1000);
    }
    return NULL;
}

static int db_exec(const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int db_init(void) {
    ensure_parent_dir(g_db_path);
    if (sqlite3_open(g_db_path, &g_db) != SQLITE_OK) return -1;
    if (db_exec("CREATE TABLE IF NOT EXISTS scores (id INTEGER PRIMARY KEY AUTOINCREMENT, game TEXT NOT NULL, username TEXT NOT NULL, score INTEGER NOT NULL, created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')));") < 0) return -1;
    if (db_exec("CREATE TABLE IF NOT EXISTS ama (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL, message TEXT NOT NULL, created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')));") < 0) return -1;
    if (db_exec("CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT NOT NULL);") < 0) return -1;
    if (db_exec("INSERT OR IGNORE INTO config(key,value) VALUES ('hit_count','0');") < 0) return -1;
    return 0;
}

static int handle_system(char *out, int cap) {
    MetricCache m;
    int p = 0;
    pthread_mutex_lock(&g_metrics.lock);
    m = g_metrics;
    pthread_mutex_unlock(&g_metrics.lock);

    p = appendf(out, p, cap, "{\"ok\":true,\"data\":{");
    p = appendf(out, p, cap, "\"hostname\":\"%s\",", m.hostname[0] ? m.hostname : "mOSs");
    p = appendf(out, p, cap, "\"kernel\":\"%s\",", m.kernel[0] ? m.kernel : "Linux");
    p = appendf(out, p, cap, "\"uptime\":%llu,", (unsigned long long)m.uptime_s);
    p = appendf(out, p, cap, "\"cpu\":{\"model\":\"%s\",\"pct\":%d,\"temp\":%d,\"freq\":%d},", m.cpu_model[0] ? m.cpu_model : "Unknown", m.cpu_pct, m.cpu_temp_c, m.cpu_freq_mhz);
    p = appendf(out, p, cap, "\"mem\":{\"total\":%llu,\"used\":%llu,\"pct\":%d},", (unsigned long long)m.mem_total_kb, (unsigned long long)m.mem_used_kb, m.mem_pct);
    p = appendf(out, p, cap, "\"disk\":{\"total\":%llu,\"used\":%llu,\"pct\":%d}", (unsigned long long)m.disk_total_mb, (unsigned long long)m.disk_used_mb, m.disk_pct);
    p = appendf(out, p, cap, "}}}");
    return p;
}

static int handle_scores(const char *req, char *out, int cap) {
    char game[64] = "";
    sqlite3_stmt *stmt = NULL;
    int p = 0, first = 1;
    json_get_str(req, "game", game, sizeof(game));
    if (sqlite3_prepare_v2(g_db, "SELECT username, score, created_at FROM scores WHERE game = ? ORDER BY score DESC, created_at ASC LIMIT 10", -1, &stmt, NULL) != SQLITE_OK) {
        return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"prepare failed\"}");
    }
    sqlite3_bind_text(stmt, 1, game, -1, SQLITE_TRANSIENT);
    p = appendf(out, p, cap, "{\"ok\":true,\"data\":[");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char user[128];
        char escaped[256];
        const unsigned char *u = sqlite3_column_text(stmt, 0);
        sqlite3_int64 score = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 ts = sqlite3_column_int64(stmt, 2);
        snprintf(user, sizeof(user), "%s", u ? (const char *)u : "guest");
        json_escape(user, escaped, sizeof(escaped));
        if (!first) p = appendf(out, p, cap, ",");
        first = 0;
        p = appendf(out, p, cap, "{\"user\":\"%s\",\"score\":%lld,\"ts\":%lld}", escaped, (long long)score, (long long)ts);
    }
    sqlite3_finalize(stmt);
    p = appendf(out, p, cap, "]}");
    return p;
}

static int handle_submit(const char *req, char *out, int cap) {
    char game[64] = "";
    char user[64] = "guest";
    long long score;
    sqlite3_stmt *stmt = NULL;
    json_get_str(req, "game", game, sizeof(game));
    json_get_str(req, "user", user, sizeof(user));
    score = json_get_int(req, "score", 0);
    if (sqlite3_prepare_v2(g_db, "INSERT INTO scores(game, username, score) VALUES(?,?,?)", -1, &stmt, NULL) != SQLITE_OK) {
        return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"prepare failed\"}");
    }
    sqlite3_bind_text(stmt, 1, game, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, score);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return appendf(out, 0, cap, "{\"ok\":true}");
}

static int handle_ama(const char *req, char *out, int cap) {
    char user[64] = "guest";
    char message[2048] = "";
    sqlite3_stmt *stmt = NULL;
    json_get_str(req, "user", user, sizeof(user));
    json_get_str(req, "message", message, sizeof(message));
    if (sqlite3_prepare_v2(g_db, "INSERT INTO ama(username, message) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK) {
        return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"prepare failed\"}");
    }
    sqlite3_bind_text(stmt, 1, user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, message, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return appendf(out, 0, cap, "{\"ok\":true,\"data\":\"saved\"}");
}

static int handle_config_get(const char *req, char *out, int cap) {
    char key[128] = "";
    sqlite3_stmt *stmt = NULL;
    json_get_str(req, "key", key, sizeof(key));
    if (sqlite3_prepare_v2(g_db, "SELECT value FROM config WHERE key = ?", -1, &stmt, NULL) != SQLITE_OK) {
        return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"prepare failed\"}");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *val = sqlite3_column_text(stmt, 0);
        char escaped[1024];
        json_escape(val ? (const char *)val : "", escaped, sizeof(escaped));
        sqlite3_finalize(stmt);
        return appendf(out, 0, cap, "{\"ok\":true,\"data\":\"%s\"}", escaped);
    }
    sqlite3_finalize(stmt);
    return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"not found\"}");
}

static int handle_config_set(const char *req, char *out, int cap) {
    char key[128] = "";
    char value[1024] = "";
    sqlite3_stmt *stmt = NULL;
    json_get_str(req, "key", key, sizeof(key));
    json_get_str(req, "value", value, sizeof(value));
    if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO config(key, value) VALUES(?,?)", -1, &stmt, NULL) != SQLITE_OK) {
        return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"prepare failed\"}");
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return appendf(out, 0, cap, "{\"ok\":true}");
}

static int handle_hits(char *out, int cap) {
    sqlite3_stmt *stmt = NULL;
    long long hits = 0;
    db_exec("UPDATE config SET value = CAST(CAST(value AS INTEGER) + 1 AS TEXT) WHERE key = 'hit_count';");
    if (sqlite3_prepare_v2(g_db, "SELECT value FROM config WHERE key = 'hit_count'", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(stmt, 0);
            hits = v ? atoll((const char *)v) : 0;
        }
        sqlite3_finalize(stmt);
    }
    return appendf(out, 0, cap, "{\"ok\":true,\"data\":%lld}", hits);
}

static int dispatch(const char *req, char *out, int cap) {
    char cmd[64] = "";
    json_get_str(req, "cmd", cmd, sizeof(cmd));
    if (strcmp(cmd, "system") == 0) return handle_system(out, cap);
    if (strcmp(cmd, "scores") == 0) return handle_scores(req, out, cap);
    if (strcmp(cmd, "submit") == 0) return handle_submit(req, out, cap);
    if (strcmp(cmd, "ama") == 0) return handle_ama(req, out, cap);
    if (strcmp(cmd, "config") == 0) return handle_config_get(req, out, cap);
    if (strcmp(cmd, "setcfg") == 0) return handle_config_set(req, out, cap);
    if (strcmp(cmd, "hits") == 0) return handle_hits(out, cap);
    return appendf(out, 0, cap, "{\"ok\":false,\"error\":\"unknown command\"}");
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char *req = calloc(1, BUF_SIZE);
    char *rsp = calloc(1, BUF_SIZE);
    ssize_t n;
    if (!req || !rsp) {
        close(fd);
        free(req);
        free(rsp);
        return NULL;
    }
    n = recv(fd, req, BUF_SIZE - 1, 0);
    if (n > 0) {
        int rlen;
        req[n] = '\0';
        rlen = dispatch(req, rsp, BUF_SIZE);
        if (rlen < BUF_SIZE - 1) rsp[rlen++] = '\n';
        send(fd, rsp, (size_t)rlen, MSG_NOSIGNAL);
    }
    close(fd);
    free(req);
    free(rsp);
    return NULL;
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_server_fd >= 0) shutdown(g_server_fd, SHUT_RDWR);
}

static void daemonize_self(void) {
    pid_t pid = fork();
    int fd;
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    if (setsid() < 0) _exit(1);
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    ensure_parent_dir(g_pid_path);
    {
        FILE *pf = fopen(g_pid_path, "w");
        if (pf) {
            fprintf(pf, "%d\n", (int)getpid());
            fclose(pf);
        }
    }
}

int main(int argc, char **argv) {
    int daemonize_flag = 0;
    pthread_t pt;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            strncpy(g_socket_path, argv[++i], sizeof(g_socket_path) - 1);
            g_socket_path[sizeof(g_socket_path) - 1] = '\0';
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            strncpy(g_db_path, argv[++i], sizeof(g_db_path) - 1);
            g_db_path[sizeof(g_db_path) - 1] = '\0';
        } else if (strcmp(argv[i], "--daemonize") == 0) {
            daemonize_flag = 1;
        }
    }

    if (daemonize_flag) daemonize_self();

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (db_init() < 0) {
        fprintf(stderr, "[moss-bridge] database init failed\n");
        return 1;
    }

    pthread_create(&pt, NULL, poll_thread, NULL);

    unlink(g_socket_path);
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket");
        g_running = 0;
    }

    if (g_running) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);
        ensure_parent_dir(g_socket_path);
        if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            g_running = 0;
        } else {
            chmod(g_socket_path, 0666);
            if (listen(g_server_fd, MAX_CLIENTS) < 0) {
                perror("listen");
                g_running = 0;
            }
        }
    }

    while (g_running) {
        int cfd = accept(g_server_fd, NULL, NULL);
        if (cfd < 0) {
            if (g_running) perror("accept");
            break;
        }
        {
            pthread_t tid;
            pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)cfd);
            pthread_detach(tid);
        }
    }

    g_running = 0;
    shutdown(g_server_fd, SHUT_RDWR);
    close(g_server_fd);
    unlink(g_socket_path);
    unlink(g_pid_path);
    pthread_join(pt, NULL);
    if (g_db) sqlite3_close(g_db);
    return 0;
}

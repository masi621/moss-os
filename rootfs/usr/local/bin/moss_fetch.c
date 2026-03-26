/* =============================================================================
 *  mOSs-Native :: moss_fetch.c
 *  moss-fetch — standalone CLI system info display.
 *
 *  Mirrors the web 'fetch' command in index.html.
 *  Queries moss-bridge via Unix socket; falls back to direct /proc reads.
 *
 *  Usage:
 *    moss-fetch               standard neofetch layout
 *    moss-fetch --json        JSON output for scripting
 *    moss-fetch --color none  disable colour output
 *    moss-fetch --cpu         CPU info only
 *    moss-fetch --mem         memory info only
 *
 *  Compile (standalone):
 *    gcc -O2 moss_fetch.c -lm -o moss-fetch
 * =============================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <math.h>

#define BRIDGE_SOCK  "/run/moss-bridge.sock"
#define ESC_RESET    "\033[0m"
#define ESC_BOLD     "\033[1m"
#define ESC_GREEN    "\033[32m"
#define ESC_BGREEN   "\033[1;32m"
#define ESC_DGREEN   "\033[2;32m"
#define ESC_CYAN     "\033[36m"
#define ESC_YELLOW   "\033[33m"
#define ESC_RED      "\033[31m"

static int  g_color  = 1;
static int  g_json   = 0;
static int  g_only   = 0;   /* 1=cpu, 2=mem */

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Bridge query                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */
static int bridge_query(const char *req, char *out, int cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BRIDGE_SOCK, sizeof addr.sun_path-1);
    if (connect(fd,(struct sockaddr*)&addr,sizeof addr)<0){close(fd);return -1;}
    send(fd, req, strlen(req), 0);
    int n = recv(fd, out, cap-1, 0); close(fd);
    if (n < 0) return -1;
    out[n] = 0; return n;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Mini JSON field extractor                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */
static int jstr(const char *json, const char *key, char *val, int vlen) {
    char needle[128]; snprintf(needle, sizeof needle, "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    int i = 0;
    while (*p && *p != '"' && i < vlen-1) {
        if (*p=='\\') p++;
        val[i++] = *p++;
    }
    val[i] = 0; return 1;
}

static long long jint(const char *json, const char *key) {
    char needle[128]; snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p==' ') p++;
    return atoll(p);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Direct /proc fallback                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */
static char *rf(const char *path) {
    FILE *f = fopen(path,"r"); if (!f) return NULL;
    char *b = malloc(4096); if (!b){fclose(f);return NULL;}
    size_t n = fread(b,1,4095,f); b[n]=0; fclose(f); return b;
}

static void fallback_info(char *hostname, char *cpu_model,
                           long long *uptime_s, long long *mem_total_kb,
                           long long *mem_used_kb, int *cpu_pct, int *mem_pct) {
    char *h = rf("/proc/sys/kernel/hostname");
    if (h) { strncpy(hostname,h,63); hostname[strcspn(hostname,"\n")]=0; free(h); }

    char *ci = rf("/proc/cpuinfo");
    if (ci) {
        char *mn = strstr(ci,"model name");
        if (mn) {
            char *co = strchr(mn,':');
            if (co) {
                co++; while(*co==' ')co++;
                char *nl=strchr(co,'\n'); if(nl)*nl=0;
                strncpy(cpu_model, co, 127);
            }
        }
        free(ci);
    }

    char *u = rf("/proc/uptime");
    if (u) { double up; sscanf(u,"%lf",&up); *uptime_s=(long long)up; free(u); }

    char *mi = rf("/proc/meminfo");
    if (mi) {
        long long total=0, avail=0, free_k=0, buf=0, cached=0;
        char k[64]; long long v;
        char *p = mi;
        while (sscanf(p,"%63s %lld kB\n",k,&v)==2) {
            if (!strcmp(k,"MemTotal:"))     total=v;
            if (!strcmp(k,"MemFree:"))      free_k=v;
            if (!strcmp(k,"MemAvailable:")) avail=v;
            if (!strcmp(k,"Buffers:"))      buf=v;
            if (!strcmp(k,"Cached:"))       cached=v;
            p=strchr(p,'\n'); if(!p)break; p++;
        }
        free(mi);
        *mem_total_kb = total;
        *mem_used_kb  = total - free_k - buf - cached;
        if (*mem_used_kb > total) *mem_used_kb = total - avail;
        if (total>0) *mem_pct = (int)((*mem_used_kb*100)/total);
    }
    (void)cpu_pct;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Bar renderer                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */
static void print_bar(int pct, int width, const char *color) {
    int fill = (pct * width) / 100;
    if (g_color) printf("%s", color);
    putchar('[');
    for (int i = 0; i < width; i++)
        putchar(i < fill ? '=' : ' ');
    putchar(']');
    printf(" %3d%%", pct);
    if (g_color) printf(ESC_RESET);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Format helpers                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */
static void fmt_uptime(long long s, char *buf, int n) {
    long long d=s/86400, h=(s%86400)/3600, m=(s%3600)/60;
    if (d) snprintf(buf,n,"%lldd %lldh %lldm",(long long)d,(long long)h,(long long)m);
    else   snprintf(buf,n,"%lldh %lldm %llds",
                    (long long)h,(long long)m,(long long)(s%60));
}

static void fmt_bytes(long long kb, char *buf, int n) {
    double mb = kb / 1024.0, gb = mb / 1024.0;
    if (gb >= 1.0) snprintf(buf,n,"%.2f GiB",gb);
    else           snprintf(buf,n,"%.0f MiB",mb);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  ASCII eye logo (small version)                                               */
/* ─────────────────────────────────────────────────────────────────────────── */
static const char *EYE_ART[] = {
    "   .  .  .  .  .  .  .",
    "  .                   .",
    " .    .----------.    .",
    ".    /  .-------.  \\   .",
    "    |  | (O) (O) |  |  ",
    ".    \\  '-------'  /   .",
    " .    '----------'    .",
    "  .                   .",
    "   .  .  .  .  .  .  .",
    "       m O S s",
};
#define EYE_LINES 10

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Info lines                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */
#define C(s)  (g_color ? s : "")

static void print_fetch(const char *json) {
    char hostname[64]="mOSs", cpu[128]="Unknown";
    long long uptime_s=0, mem_total=0, mem_used=0;
    int cpu_pct=0, mem_pct=0, cpu_temp=-1, cpu_freq=-1, disk_pct=0;
    long long disk_total=0, disk_used=0;

    if (json && strstr(json,"\"ok\":true")) {
        jstr(json,"hostname",hostname,sizeof hostname);
        /* CPU model is nested; find it manually */
        const char *cpu_sec = strstr(json,"\"cpu\":{");
        if (cpu_sec) jstr(cpu_sec,"model",cpu,sizeof cpu);
        uptime_s  = jint(json,"uptime");
        cpu_pct   = (int)jint(json,"pct");
        cpu_temp  = (int)jint(json,"temp");
        cpu_freq  = (int)jint(json,"freq");
        const char *mem_sec = strstr(json,"\"mem\":{");
        if (mem_sec) {
            mem_total = jint(mem_sec,"total");  /* kB */
            mem_used  = jint(mem_sec,"used");
            mem_pct   = (int)jint(mem_sec,"pct");
        }
        const char *disk_sec = strstr(json,"\"disk\":{");
        if (disk_sec) {
            disk_total = jint(disk_sec,"total");
            disk_used  = jint(disk_sec,"used");
            disk_pct   = (int)jint(disk_sec,"pct");
        }
    } else {
        fallback_info(hostname, cpu, &uptime_s,
                      &mem_total, &mem_used, &cpu_pct, &mem_pct);
    }

    char upbuf[64]; fmt_uptime(uptime_s, upbuf, sizeof upbuf);
    char mem_total_s[32], mem_used_s[32];
    fmt_bytes(mem_total, mem_total_s, sizeof mem_total_s);
    fmt_bytes(mem_used,  mem_used_s,  sizeof mem_used_s);

    /* Info lines parallel to ASCII art */
    const char *info[EYE_LINES];
    char lines[EYE_LINES][256];
    int  li = 0;

    snprintf(lines[li], 256, "%s%s%s@%smOSs%s",
             C(ESC_BGREEN), hostname, C(ESC_RESET),
             C(ESC_BGREEN), C(ESC_RESET));   info[li] = lines[li]; li++;
    snprintf(lines[li], 256, "%s──────────────────────────────────────%s",
             C(ESC_DGREEN), C(ESC_RESET));   info[li] = lines[li]; li++;
    snprintf(lines[li], 256, "%sOS%s     : mOSs Native v1.0",
             C(ESC_GREEN), C(ESC_RESET));    info[li] = lines[li]; li++;
    snprintf(lines[li], 256, "%sKernel%s : Linux 6.6 LTS",
             C(ESC_GREEN), C(ESC_RESET));    info[li] = lines[li]; li++;
    snprintf(lines[li], 256, "%sUptime%s : %s",
             C(ESC_GREEN), C(ESC_RESET), upbuf); info[li] = lines[li]; li++;
    snprintf(lines[li], 256, "%sCPU%s    : %.60s (%d%%)",
             C(ESC_GREEN), C(ESC_RESET), cpu, cpu_pct); info[li] = lines[li]; li++;

    if (cpu_temp >= 0)
        snprintf(lines[li], 256, "%sTemp%s   : %d°C",
                 C(ESC_GREEN), C(ESC_RESET), cpu_temp);
    else
        snprintf(lines[li], 256, "%sTemp%s   : N/A",
                 C(ESC_GREEN), C(ESC_RESET));
    info[li] = lines[li]; li++;

    snprintf(lines[li], 256, "%sMemory%s : %s / %s (%d%%)",
             C(ESC_GREEN), C(ESC_RESET),
             mem_used_s, mem_total_s, mem_pct); info[li] = lines[li]; li++;

    snprintf(lines[li], 256, "%sDisk%s   : %lld MB / %lld MB (%d%%)",
             C(ESC_GREEN), C(ESC_RESET),
             (long long)disk_used, (long long)disk_total, disk_pct);
    info[li] = lines[li]; li++;

    snprintf(lines[li], 256, "%sShell%s  : moss-shell v1.0",
             C(ESC_GREEN), C(ESC_RESET)); info[li] = lines[li]; li++;

    /* Print side-by-side */
    for (int i = 0; i < EYE_LINES; i++) {
        if (g_color) printf(ESC_GREEN);
        printf("  %-24s", EYE_ART[i]);
        if (g_color) printf(ESC_RESET);
        if (i < li) printf("  %s", info[i]);
        putchar('\n');
    }

    /* Progress bars */
    printf("\n");
    printf("  %sCPU %s ", C(ESC_GREEN), C(ESC_RESET));
    print_bar(cpu_pct, 30, ESC_GREEN); printf("\n");
    printf("  %sMEM %s ", C(ESC_CYAN),  C(ESC_RESET));
    print_bar(mem_pct, 30, ESC_CYAN);  printf("\n");
    printf("  %sDSK %s ", C(ESC_YELLOW),C(ESC_RESET));
    print_bar(disk_pct,30, ESC_YELLOW);printf("\n\n");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Main                                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--json"))        g_json=1;
        else if (!strcmp(argv[i],"--cpu"))    g_only=1;
        else if (!strcmp(argv[i],"--mem"))    g_only=2;
        else if (!strcmp(argv[i],"--color") && i+1<argc) {
            g_color = strcmp(argv[++i],"none")!=0;
        }
    }

    char buf[65536]={0};
    bridge_query("{\"cmd\":\"system\"}", buf, sizeof buf);
    print_fetch(buf[0] ? buf : NULL);
    return 0;
}

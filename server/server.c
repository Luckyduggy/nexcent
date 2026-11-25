#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define MAX_REQUEST 65536
#define PBKDF2_ITER 120000
#define WORK_QUEUE_SIZE 512
#define MAX_WORKERS 32

const char *db_path;
long long started_ms;
int requests, active, worker_count;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

void statsSnapshot(int *request_count, int *active_count) {
    pthread_mutex_lock(&stats_mutex);
    if (request_count) *request_count = requests;
    if (active_count) *active_count = active;
    pthread_mutex_unlock(&stats_mutex);
}

void statsAddRequest(void) {
    pthread_mutex_lock(&stats_mutex);
    requests++;
    pthread_mutex_unlock(&stats_mutex);
}

void statsChangeActive(int change) {
    pthread_mutex_lock(&stats_mutex);
    active += change;
    pthread_mutex_unlock(&stats_mutex);
}

long long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

double monoMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

char *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        if (size) *size = n;
    }
    return buf;
}

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
} sha_t;

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

void shaBlock(sha_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void shaInit(sha_t *s) {
    const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
}

void shaAdd(sha_t *s, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n > 0) {
        size_t used = s->len % 64, take = 64 - used < n ? 64 - used : n;
        memcpy(s->buf + used, p, take);
        s->len += take;
        p += take;
        n -= take;
        if (s->len % 64 == 0) shaBlock(s, s->buf);
    }
}

void shaEnd(sha_t *s, uint8_t out[32]) {
    uint8_t zeros[64] = {0};
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, len8[8];
    size_t z = (119 - s->len % 64) % 64;
    shaAdd(s, &pad, 1);
    shaAdd(s, zeros, z);
    for (int i = 0; i < 8; i++) len8[i] = bits >> (56 - 8 * i);
    shaAdd(s, len8, 8);
    for (int i = 0; i < 32; i++) out[i] = s->h[i / 4] >> (24 - 8 * (i % 4));
}

void hmacSha256(const sha_t *in, const sha_t *out, const void *msg, size_t n, uint8_t d[32]) {
    sha_t s = *in;
    shaAdd(&s, msg, n);
    shaEnd(&s, d);
    s = *out;
    shaAdd(&s, d, 32);
    shaEnd(&s, d);
}

void pbkdf2(const char *pw, const uint8_t *salt, size_t salt_len, int iter, uint8_t key[32]) {
    uint8_t k[64] = {0}, pad[64], u[32], msg[80];
    sha_t in, out;
    size_t n = strlen(pw);
    if (n > 64) {
        sha_t s;
        shaInit(&s);
        shaAdd(&s, pw, n);
        shaEnd(&s, k);
    } else {
        memcpy(k, pw, n);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    shaInit(&in);
    shaAdd(&in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    shaInit(&out);
    shaAdd(&out, pad, 64);

    memcpy(msg, salt, salt_len);
    memcpy(msg + salt_len, "\0\0\0\1", 4);
    hmacSha256(&in, &out, msg, salt_len + 4, u);
    memcpy(key, u, 32);
    for (int i = 1; i < iter; i++) {
        hmacSha256(&in, &out, u, 32, u);
        for (int j = 0; j < 32; j++) key[j] ^= u[j];
    }
}

void toHex(const uint8_t *d, int n, char *out) {
    for (int i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

int fromHex(const char *s, uint8_t *out) {
    int n = (int)strlen(s) / 2;
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return n;
}

void randomBytes(uint8_t *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(p, 1, n, f) != n) {
        perror("/dev/urandom");
        exit(1);
    }
    fclose(f);
}

const char *arg_text(sqlite3_value *v) { return (const char *)sqlite3_value_text(v); }

void sqlSha256(sqlite3_context *c, int argc, sqlite3_value **v) {
    const char *s = arg_text(v[0]);
    uint8_t d[32];
    char hex[65];
    sha_t h;
    if (!s) {
        sqlite3_result_null(c);
        return;
    }
    shaInit(&h);
    shaAdd(&h, s, strlen(s));
    shaEnd(&h, d);
    toHex(d, 32, hex);
    sqlite3_result_text(c, hex, -1, SQLITE_TRANSIENT);
}

void sqlHashPassword(sqlite3_context *c, int argc, sqlite3_value **v) {
    const char *pw = arg_text(v[0]);
    uint8_t salt[16], key[32];
    char salt_hex[33], key_hex[65], out[160];
    if (!pw) {
        sqlite3_result_null(c);
        return;
    }
    randomBytes(salt, sizeof salt);
    pbkdf2(pw, salt, sizeof salt, PBKDF2_ITER, key);
    toHex(salt, 16, salt_hex);
    toHex(key, 32, key_hex);
    snprintf(out, sizeof out, "pbkdf2-sha256$%d$%s$%s", PBKDF2_ITER, salt_hex, key_hex);
    sqlite3_result_text(c, out, -1, SQLITE_TRANSIENT);
}

void sqlCheckPassword(sqlite3_context *c, int argc, sqlite3_value **v) {
    const char *pw = arg_text(v[0]), *stored = arg_text(v[1]);
    char salt_hex[65], key_hex[65];
    uint8_t salt[32], want[32], got[32];
    int iter, ok = 0;
    if (pw && stored &&
        sscanf(stored, "pbkdf2-sha256$%d$%64[0-9a-f]$%64[0-9a-f]", &iter, salt_hex, key_hex) == 3 &&
        iter > 0 && strlen(key_hex) == 64) {
        int n = fromHex(salt_hex, salt);
        fromHex(key_hex, want);
        pbkdf2(pw, salt, n, iter, got);
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= got[i] ^ want[i];
        ok = diff == 0;
    }
    sqlite3_result_int(c, ok);
}

void sqlSlugify(sqlite3_context *c, int argc, sqlite3_value **v) {
    const unsigned char *s = sqlite3_value_text(v[0]);
    char out[96];
    int k = 0;
    for (; s && *s && k < 80; s++) {
        if (*s < 128 && isalnum(*s)) out[k++] = (char)tolower(*s);
        else if (k > 0 && out[k - 1] != '-') out[k++] = '-';
    }
    while (k > 0 && out[k - 1] == '-') k--;
    if (k == 0) {
        strcpy(out, "post");
        k = 4;
    }
    sqlite3_result_text(c, out, k, SQLITE_TRANSIENT);
}

void *bench_job(void *arg) {
    uint8_t salt[16] = {0}, key[32];
    double t0 = monoMs();
    pbkdf2("benchmark", salt, sizeof salt, PBKDF2_ITER, key);
    *(double *)arg = monoMs() - t0;
    return NULL;
}

void sqlBenchmark(sqlite3_context *c, int argc, sqlite3_value **v) {
    int n = sqlite3_value_int(v[0]);
    pthread_t th[16];
    double per[16], single = 1e18, t, wall;
    if (n < 1 || n > 16) n = 4;
    for (int i = 0; i < 3; i++) {
        bench_job(&t);
        if (t < single) single = t;
    }
    wall = monoMs();
    int started = 0;
    while (started < n && pthread_create(&th[started], NULL, bench_job, &per[started]) == 0) started++;
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    wall = monoMs() - wall;

    char out[640];
    int len = snprintf(out, sizeof out,
                       "{\"threads\":%d,\"cpus\":%ld,\"single_ms\":%.1f,\"parallel_wall_ms\":%.1f,"
                       "\"sequential_estimate_ms\":%.1f,\"speedup\":%.2f,\"per_thread_ms\":[",
                       started, sysconf(_SC_NPROCESSORS_ONLN), single, wall, single * started,
                       wall > 0 ? single * started / wall : 0.0);
    for (int i = 0; i < started; i++) len += snprintf(out + len, sizeof out - len, "%s%.1f", i ? "," : "", per[i]);
    snprintf(out + len, sizeof out - len, "]}");
    sqlite3_result_text(c, out, -1, SQLITE_TRANSIENT);
}

sqlite3 *open_db(void) {
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", 0, 0, 0);
    sqlite3_create_function(db, "sha256", 1, SQLITE_UTF8, 0, sqlSha256, 0, 0);
    sqlite3_create_function(db, "hash_password", 1, SQLITE_UTF8, 0, sqlHashPassword, 0, 0);
    sqlite3_create_function(db, "check_password", 2, SQLITE_UTF8, 0, sqlCheckPassword, 0, 0);
    sqlite3_create_function(db, "slugify", 1, SQLITE_UTF8, 0, sqlSlugify, 0, 0);
    sqlite3_create_function(db, "benchmark", 1, SQLITE_UTF8, 0, sqlBenchmark, 0, 0);
    return db;
}

typedef struct {
    int fd, uid;
    char method[8], path[256], token[128], fresh[65], ip[INET_ADDRSTRLEN];
    char *query, *param, *body, *pre;
} req_t;

req_t *request_queue[WORK_QUEUE_SIZE];
size_t queue_head, queue_tail, queue_count;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_ready = PTHREAD_COND_INITIALIZER;

int queuePush(req_t *r) {
    pthread_mutex_lock(&queue_mutex);
    if (queue_count == WORK_QUEUE_SIZE) {
        pthread_mutex_unlock(&queue_mutex);
        return 0;
    }
    request_queue[queue_tail] = r;
    queue_tail = (queue_tail + 1) % WORK_QUEUE_SIZE;
    queue_count++;
    pthread_cond_signal(&queue_ready);
    pthread_mutex_unlock(&queue_mutex);
    return 1;
}

req_t *queuePop(void) {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == 0) pthread_cond_wait(&queue_ready, &queue_mutex);
    req_t *r = request_queue[queue_head];
    queue_head = (queue_head + 1) % WORK_QUEUE_SIZE;
    queue_count--;
    pthread_mutex_unlock(&queue_mutex);
    return r;
}

int queueLength(void) {
    pthread_mutex_lock(&queue_mutex);
    int count = (int)queue_count;
    pthread_mutex_unlock(&queue_mutex);
    return count;
}

long qint(const char *q, const char *key, long def, long lo, long hi) {
    size_t n = strlen(key);
    for (const char *p = q; (p = strstr(p, key)) != NULL; p += n)
        if ((p == q || p[-1] == '&') && p[n] == '=') {
            long v = strtol(p + n + 1, NULL, 10);
            return v < lo ? lo : v > hi ? hi : v;
        }
    return def;
}

void bindParams(sqlite3_stmt *st, const req_t *r) {
#define TXT(name, val) {name, (val) ? (val) : "", 0}
#define NUM(name, val) {name, NULL, (val)}
    int request_total, active_total;
    statsSnapshot(&request_total, &active_total);
    const struct {
        const char *name, *text;
        long long num;
    } p[] = {
        TXT(":body", r->body),
        TXT(":p", r->param),
        TXT(":token", r->token),
        TXT(":fresh", r->fresh),
        TXT(":pre", r->pre),
        TXT(":ip", r->ip),
        NUM(":uid", r->uid),
        NUM(":now", nowMs()),
        NUM(":limit", qint(r->query, "limit", 20, 1, 100)),
        NUM(":offset", qint(r->query, "offset", 0, 0, 1000000)),
        NUM(":unread", qint(r->query, "unread", 0, 0, 1)),
        NUM(":n", qint(r->query, "n", 4, 1, 16)),
        NUM(":pid", getpid()),
        NUM(":started", started_ms),
        NUM(":requests", request_total),
        NUM(":threads", active_total),
        NUM(":queue", queueLength()),
        NUM(":pool_size", worker_count),
        NUM(":cpus", sysconf(_SC_NPROCESSORS_ONLN)),
    };
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) {
        int k = sqlite3_bind_parameter_index(st, p[i].name);
        if (!k) continue;
        if (p[i].text) sqlite3_bind_text(st, k, p[i].text, -1, SQLITE_STATIC);
        else sqlite3_bind_int64(st, k, p[i].num);
    }
}

int run(sqlite3 *db, const char *sql, const req_t *r, char **out) {
    int rc = SQLITE_OK;
    *out = NULL;
    while (rc == SQLITE_OK && *sql) {
        sqlite3_stmt *st;
        rc = sqlite3_prepare_v2(db, sql, -1, &st, &sql);
        if (rc != SQLITE_OK || !st) break;
        bindParams(st, r);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(st, 0);
            free(*out);
            *out = text ? strdup(text) : NULL;
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        sqlite3_finalize(st);
    }
    return rc;
}

#define CSP                                                                                         \
    "default-src 'self'; script-src 'self'; style-src 'self' https://fonts.googleapis.com; "        \
    "font-src https://fonts.gstatic.com; img-src 'self' data:; connect-src 'self'; "                \
    "frame-ancestors 'none'; base-uri 'self'; form-action 'self'"

void sendAll(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= w;
    }
}

void reply(req_t *r, int code, const char *type, const void *body, size_t len) {
    char head[768];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d \r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
                     "Content-Security-Policy: " CSP "\r\n\r\n",
                     code, type, len);
    sendAll(r->fd, head, n);
    sendAll(r->fd, body, len);
    fprintf(stderr, "%s %s -> %d\n", r->method, r->path, code);
}

void sendError(req_t *r, int code, const char *msg) {
    char body[256];
    int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", msg);
    reply(r, code, "application/json; charset=utf-8", body, n < (int)sizeof body ? n : (int)sizeof body - 1);
}

int readRequest(int fd, char *buf, size_t cap, char **body) {
    size_t n = 0;
    char *end = NULL;
    long want = 0;
    while (n < cap - 1) {
        ssize_t got = read(fd, buf + n, cap - 1 - n);
        if (got <= 0) return -1;
        n += got;
        buf[n] = 0;
        if (!end && (end = strstr(buf, "\r\n\r\n")) != NULL) {
            char *cl = strcasestr(buf, "\r\ncontent-length:");
            want = cl && cl < end ? atol(cl + 17) : 0;
            if (want < 0) return -1;
        }
        if (end && (long)(n - (end + 4 - buf)) >= want) break;
    }
    if (!end || (long)(n - (end + 4 - buf)) < want) return -2;
    *end = 0;
    *body = end + 4;
    (*body)[want] = 0;
    return 0;
}

#define OKJSON       "'{\"ok\":true}'"
#define JSON(key)    "json_extract(:body,'$." key "')"
#define TRIM(x)      "trim(" x ",' '||char(9,10,13))"
#define EMAIL        "lower(" TRIM(JSON("email")) ")"
#define COL(c)       "'" #c "'," #c

#define USER_COLS    COL(id) "," COL(name) "," COL(email) "," COL(role) "," COL(created_at)
#define CARD_COLS    COL(slug) "," COL(title) "," COL(excerpt) "," COL(cover) "," COL(author) "," COL(created_at)
#define POST_COLS    COL(id) "," COL(slug) "," COL(title) "," COL(excerpt) "," COL(cover) "," COL(author) "," \
                     COL(published) "," COL(created_at)
#define MESSAGE_COLS COL(id) "," COL(name) "," COL(email) "," COL(message) "," COL(is_read) "," COL(created_at)
#define AUDIT_COLS   COL(id) "," COL(event) "," COL(user_id) "," COL(ip) "," COL(pid) "," COL(detail) "," COL(created_at)

#define PAGE(cols, from, order)                                                                      \
    "SELECT json_object('items',json((SELECT json_group_array(json_object(" cols "))"               \
    " FROM (SELECT * FROM " from " ORDER BY " order " LIMIT :limit OFFSET :offset))),"              \
    "'total',(SELECT count(*) FROM " from "))"

#define AUDIT(event, detail)                                                                         \
    "INSERT INTO audit_log(event,user_id,ip,pid,detail) SELECT '" event "',NULLIF(:uid,0),:ip,:pid," \
    detail " WHERE changes()>0;"

#define DELETE_BY_ID(table, label) "DELETE FROM " table " WHERE id=:p RETURNING " OKJSON ";" AUDIT("admin_delete", "'" label "'")

#define CHECK(cond, msg) " WHEN NOT COALESCE(" cond ",0) THEN '" msg "'"
#define VALIDATE(...)    "SELECT CASE" __VA_ARGS__ " END"

#define V_NAME  CHECK("length(" TRIM(JSON("name")) ") BETWEEN 1 AND 80", "Name must be 1–80 characters.")
#define V_EMAIL CHECK(EMAIL " LIKE '%_@_%.__%' AND " EMAIL " NOT LIKE '% %' AND length(" EMAIL ")<255", \
                      "Enter a valid email address.")
#define V_PASS  CHECK("length(" JSON("password") ") BETWEEN 8 AND 128", "Password must be 8–128 characters.")
#define V_MSG   CHECK("length(" TRIM(JSON("message")) ") BETWEEN 1 AND 5000", "Message must be 1–5000 characters.")

#define P_TITLE   TRIM(JSON("title"))
#define P_EXCERPT TRIM("COALESCE(" JSON("excerpt") ",'')")
#define P_BODY    TRIM("COALESCE(" JSON("body") ",'')")
#define P_AUTHOR  "COALESCE(NULLIF(" TRIM(JSON("author")) ",''),'Nexcent Team')"
#define P_COVER   "COALESCE(NULLIF(" TRIM(JSON("cover")) ",''),'/images/blog-1.jpg')"
#define P_PUB     "COALESCE(" JSON("published") ",1)"
#define V_POST                                                                                         \
    CHECK("length(" P_TITLE ") BETWEEN 3 AND 160", "Title must be 3–160 characters.")                  \
    CHECK("length(" P_EXCERPT ")<=300", "Excerpt must be at most 300 characters.")                     \
    CHECK("length(" P_BODY ")<=20000", "Body must be at most 20000 characters.")                       \
    CHECK("length(" P_AUTHOR ")<=80", "Author must be at most 80 characters.")                         \
    CHECK(P_COVER " GLOB '/images/*' AND " P_COVER " NOT GLOB '*[^A-Za-z0-9._/-]*' AND " P_COVER      \
          " NOT LIKE '%..%' AND length(" P_COVER ")<=100",                                             \
          "Cover must be an image from /images/, e.g. /images/blog-1.jpg.")                            \
    CHECK(P_PUB " IN (0,1)", "Published must be true or false.")

#define SESSION_JSON                                                                                  \
    "SELECT json_object('token',:fresh,'user',json_object(" USER_COLS ")) FROM users "               \
    "WHERE id=(SELECT user_id FROM sessions WHERE token_hash=sha256(:fresh))"

#define DAILY(table)                                                                                  \
    "json((SELECT json_group_array(json_object('d',d,'c',c)) FROM (SELECT substr(created_at,1,10) d," \
    "count(*) c FROM " table " WHERE created_at>=date('now','-13 days') GROUP BY d ORDER BY d)))"

enum { PUBLIC, USER, ADMIN };

typedef struct {
    const char *method, *pattern;
    int access;
    int ok;
    const char *check;
    const char *sql;
    const char *pre;
} route_t;

const route_t routes[] = {

    {"GET", "/api/health", PUBLIC, 200, NULL,
     "SELECT json_object('status','ok','pid',:pid,'uptime_s',(:now-:started)/1000,"
     "'time',strftime('%Y-%m-%dT%H:%M:%SZ','now'))"},

    {"GET", "/api/posts", PUBLIC, 200, NULL, PAGE(CARD_COLS, "posts WHERE published=1", "created_at DESC,id DESC")},

    {"GET", "/api/posts/:slug", PUBLIC, 200, NULL,
     "SELECT json_object(" CARD_COLS "," COL(body) ") FROM posts WHERE slug=:p AND published=1"},

    {"GET", "/api/faqs", PUBLIC, 200, NULL,
     "SELECT json_object('items',json((SELECT json_group_array(json_object(" COL(question) "," COL(answer) "))"
     " FROM (SELECT * FROM faqs ORDER BY position,id))))"},

    {"POST", "/api/subscribe", PUBLIC, 200, VALIDATE(V_EMAIL),
     "INSERT OR IGNORE INTO subscribers(email) VALUES(" EMAIL "); SELECT " OKJSON},

    {"POST", "/api/contact", PUBLIC, 201, VALIDATE(V_NAME V_EMAIL V_MSG),
     "INSERT INTO messages(name,email,message) VALUES(" TRIM(JSON("name")) "," EMAIL "," TRIM(JSON("message"))
     ") RETURNING " OKJSON},

    {"POST", "/api/register", PUBLIC, 201, VALIDATE(V_NAME V_EMAIL V_PASS),
     "INSERT INTO users(name,email,password_hash) VALUES(" TRIM(JSON("name")) "," EMAIL ",:pre);"
     "INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(sha256(:fresh),last_insert_rowid(),:now+604800000);"
     "INSERT INTO audit_log(event,user_id,ip,pid) SELECT 'register',id,:ip,:pid FROM users WHERE email=" EMAIL ";"
     SESSION_JSON,
     "SELECT hash_password(" JSON("password") ")"},

    {"POST", "/api/login", PUBLIC, 200, NULL,
     "DELETE FROM sessions WHERE expires_at<:now;"
     "INSERT INTO sessions(token_hash,user_id,expires_at) SELECT sha256(:fresh),id,:now+604800000 FROM users WHERE id=:pre;"
     "INSERT INTO audit_log(event,user_id,ip,pid) SELECT CASE WHEN changes()>0 THEN 'login' ELSE 'login_fail' END,"
     "(SELECT id FROM users WHERE email=" EMAIL "),:ip,:pid;"
     SESSION_JSON,
     "SELECT id FROM users WHERE email=" EMAIL " AND check_password(" JSON("password") ",password_hash)"},

    {"GET", "/api/me", USER, 200, NULL,
     "SELECT json_object('user',json_object(" USER_COLS ")) FROM users WHERE id=:uid"},

    {"POST", "/api/logout", USER, 200, NULL,
     "DELETE FROM sessions WHERE token_hash=sha256(:token) RETURNING " OKJSON ";" AUDIT("logout", "NULL")},

    {"GET", "/api/admin/stats", ADMIN, 200, NULL,
     "SELECT json_object("
     "'counts',json_object('users',(SELECT count(*) FROM users),'subscribers',(SELECT count(*) FROM subscribers),"
     "'messages',(SELECT count(*) FROM messages),'unread',(SELECT count(*) FROM messages WHERE is_read=0),"
     "'posts',(SELECT count(*) FROM posts),'sessions',(SELECT count(*) FROM sessions WHERE expires_at>:now)),"
     "'daily',json_object('from',date('now','-13 days'),'days',14,'users'," DAILY("users")
     ",'subscribers'," DAILY("subscribers") ",'messages'," DAILY("messages") "),"
     "'workers',json_array(json_object('pid',:pid,'worker_id',1,'started_at',:started,'last_seen',:now,"
     "'requests',:requests,'threads',:threads,'pool_size',:pool_size,'queue',:queue)),"
     "'server',json_object('served_by',:pid,'cpus',:cpus,'workers',1,'threads',:threads,"
     "'thread_pool',:pool_size,'queue',:queue,'now',:now))"},

    {"GET", "/api/admin/benchmark", ADMIN, 200, NULL, "SELECT benchmark(:n)"},

    {"GET", "/api/admin/audit", ADMIN, 200, NULL, PAGE(AUDIT_COLS, "audit_log", "id DESC")},

    {"GET", "/api/admin/messages", ADMIN, 200, NULL,
     PAGE(MESSAGE_COLS, "messages WHERE (:unread=0 OR is_read=0)", "id DESC")},

    {"PATCH", "/api/admin/messages/:id", ADMIN, 200,
     VALIDATE(CHECK(JSON("is_read") " IN (0,1)", "is_read must be true or false.")),
     "UPDATE messages SET is_read=" JSON("is_read") " WHERE id=:p RETURNING " OKJSON},

    {"DELETE", "/api/admin/messages/:id", ADMIN, 200, NULL, DELETE_BY_ID("messages", "Message")},

    {"GET", "/api/admin/subscribers", ADMIN, 200, NULL,
     PAGE(COL(id) "," COL(email) "," COL(created_at), "subscribers", "id DESC")},

    {"DELETE", "/api/admin/subscribers/:id", ADMIN, 200, NULL, DELETE_BY_ID("subscribers", "Subscriber")},

    {"GET", "/api/admin/users", ADMIN, 200, NULL, PAGE(USER_COLS, "users", "id")},

    {"PATCH", "/api/admin/users/:id", ADMIN, 200,
     VALIDATE(CHECK("CAST(:p AS INT)<>:uid", "You cannot change your own role.")
              CHECK(JSON("role") " IN ('user','admin')", "Role must be user or admin.")),
     "UPDATE users SET role=" JSON("role") " WHERE id=:p RETURNING " OKJSON ";" AUDIT("role_change", JSON("role"))},

    {"DELETE", "/api/admin/users/:id", ADMIN, 200,
     VALIDATE(CHECK("CAST(:p AS INT)<>:uid", "You cannot delete your own account.")),
     DELETE_BY_ID("users", "User")},

    {"GET", "/api/admin/posts", ADMIN, 200, NULL, PAGE(POST_COLS, "posts", "id DESC")},

    {"POST", "/api/admin/posts", ADMIN, 201, VALIDATE(V_POST),
     "INSERT INTO posts(slug,title,excerpt,body,cover,author,published) SELECT"
     " CASE WHEN EXISTS(SELECT 1 FROM posts WHERE slug=s) THEN s||'-'||lower(hex(randomblob(3))) ELSE s END,"
     P_TITLE "," P_EXCERPT "," P_BODY "," P_COVER "," P_AUTHOR "," P_PUB
     " FROM (SELECT slugify(" P_TITLE ") AS s) RETURNING json_object(" POST_COLS ");"
     AUDIT("post_create", "(SELECT slug FROM posts WHERE id=last_insert_rowid())")},

    {"GET", "/api/admin/posts/:id", ADMIN, 200, NULL,
     "SELECT json_object(" POST_COLS "," COL(body) ") FROM posts WHERE id=:p"},

    {"PUT", "/api/admin/posts/:id", ADMIN, 200, VALIDATE(V_POST),
     "UPDATE posts SET title=" P_TITLE ",excerpt=" P_EXCERPT ",body=" P_BODY ",cover=" P_COVER
     ",author=" P_AUTHOR ",published=" P_PUB " WHERE id=:p RETURNING " OKJSON ";" AUDIT("post_update", ":p")},

    {"DELETE", "/api/admin/posts/:id", ADMIN, 200, NULL, DELETE_BY_ID("posts", "Post")},
};

int match(const char *pattern, char *path, char **param) {
    const char *colon = strchr(pattern, ':');
    if (!colon) return strcmp(pattern, path) == 0;
    size_t n = colon - pattern;
    if (strncmp(pattern, path, n) != 0 || !path[n] || strchr(path + n, '/')) return 0;
    *param = path + n;
    return 1;
}

void api(req_t *r) {
    const route_t *rt = NULL;
    int path_known = 0;
    for (size_t i = 0; i < sizeof routes / sizeof *routes && !rt; i++)
        if (match(routes[i].pattern, r->path, &r->param)) {
            path_known = 1;
            if (strcmp(routes[i].method, r->method) == 0) rt = &routes[i];
        }
    if (!rt) {
        if (path_known) sendError(r, 405, "Method not allowed");
        else sendError(r, 404, "Not found");
        return;
    }

    sqlite3 *db = open_db();
    char *out = NULL, *who = NULL, *bad = NULL;
    int rc = SQLITE_OK;
    int is_get = strcmp(r->method, "GET") == 0;
    int proceed = db != NULL;
    if (!db) {
        sendError(r, 500, "Database error");
        return;
    }
    if (strstr(rt->sql, ":fresh")) {
        uint8_t rnd[32];
        randomBytes(rnd, sizeof rnd);
        toHex(rnd, sizeof rnd, r->fresh);
    }

    if (rt->access != PUBLIC) {
        char role[16] = "";
        run(db, "SELECT u.id||','||u.role FROM sessions s JOIN users u ON u.id=s.user_id"
                " WHERE s.token_hash=sha256(:token) AND s.expires_at>:now", r, &who);
        if (!who) {
            sendError(r, 401, "Authentication required.");
            proceed = 0;
        } else {
            sscanf(who, "%d,%15s", &r->uid, role);
            if (rt->access == ADMIN && strcmp(role, "admin") != 0) {
                sendError(r, 403, "Administrator access required.");
                proceed = 0;
            }
        }
    }

    if (proceed && rt->check) {
        rc = run(db, rt->check, r, &bad);
        if (rc != SQLITE_OK) {
            sendError(r, 400, "Invalid JSON body.");
            proceed = 0;
        } else if (bad) {
            sendError(r, 422, bad);
            proceed = 0;
        }
    }

    if (proceed && rt->pre) {
        rc = run(db, rt->pre, r, &r->pre);
        if (rc != SQLITE_OK) {
            sendError(r, 400, "Invalid JSON body.");
            proceed = 0;
        }
    }

    if (proceed) {
        if (!is_get) sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);
        rc = run(db, rt->sql, r, &out);
        if (!is_get) sqlite3_exec(db, rc == SQLITE_OK ? "COMMIT" : "ROLLBACK", 0, 0, 0);

        if (rc == SQLITE_CONSTRAINT) {
            sendError(r, 409, "An account with this email already exists.");
        } else if (rc != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
            if (rc == SQLITE_ERROR) sendError(r, 400, "Invalid JSON body.");
            else sendError(r, 500, "Internal server error");
        } else if (!out) {
            if (strcmp(rt->pattern, "/api/login") == 0) sendError(r, 401, "Invalid email or password.");
            else sendError(r, 404, "Not found");
        } else {
            reply(r, rt->ok, "application/json; charset=utf-8", out, strlen(out));
        }
    }

    free(out);
    free(who);
    free(bad);
    free(r->pre);
    r->pre = NULL;
    sqlite3_close(db);
}

void sendNotFound(req_t *r) {
    size_t size = 0;
    char *data = slurp("pages/404.html", &size);
    if (data) {
        reply(r, 404, "text/html; charset=utf-8", data, size);
        free(data);
    } else {
        sendError(r, 404, "Not found");
    }
}

void sendFile(req_t *r) {
    const char *dirs[] = {"/pages/", "/css/", "/js/", "/images/"};
    const struct {
        const char *ext, *type;
    } mime[] = {{".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
                {".js", "text/javascript; charset=utf-8"}, {".jpg", "image/jpeg"}, {".png", "image/png"},
                {".svg", "image/svg+xml"}, {".webp", "image/webp"}, {".ico", "image/x-icon"}};
    const char *p = strcmp(r->path, "/") == 0 ? "/index.html" : r->path;
    int allowed = strcmp(p, "/index.html") == 0;
    for (int i = 0; i < 4; i++)
        if (strncmp(p, dirs[i], strlen(dirs[i])) == 0) allowed = 1;

    struct stat st;
    size_t size = 0;
    char *data = NULL;
    if (allowed && !strstr(p, "..") && stat(p + 1, &st) == 0 && S_ISREG(st.st_mode)) data = slurp(p + 1, &size);
    if (!data) {
        sendNotFound(r);
        return;
    }
    const char *dot = strrchr(p, '.'), *type = "application/octet-stream";
    for (size_t i = 0; dot && i < sizeof mime / sizeof *mime; i++)
        if (strcmp(dot, mime[i].ext) == 0) type = mime[i].type;
    reply(r, 200, type, data, size);
    free(data);
}

void *serve(void *arg) {
    req_t *r = arg;
    char *buf = malloc(MAX_REQUEST + 1);
    struct timeval timeout = {5, 0};
    setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(r->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    int got = buf ? readRequest(r->fd, buf, MAX_REQUEST, &r->body) : -1;
    if (got == -2) sendError(r, 413, "Request too large");
    if (got == 0 && sscanf(buf, "%7s %255s", r->method, r->path) == 2) {
        statsAddRequest();
        r->query = strchr(r->path, '?');
        if (r->query) *r->query++ = 0;
        else r->query = "";
        r->param = "";
        char *auth = strcasestr(buf, "\r\nauthorization: bearer ");
        if (auth) sscanf(auth + 24, "%127[^\r\n ]", r->token);

        if (strncmp(r->path, "/api/", 5) == 0) api(r);
        else if (strcmp(r->method, "GET") == 0) sendFile(r);
        else sendError(r, 405, "Method not allowed");
    }
    close(r->fd);
    free(buf);
    free(r);
    statsChangeActive(-1);
    return NULL;
}

void *workerLoop(void *arg) {
    (void)arg;
    for (;;) {
        req_t *r = queuePop();
        statsChangeActive(1);
        serve(r);
    }
    return NULL;
}

int startWorkers(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int wanted = cpus > 0 ? (int)cpus * 2 : 4;
    if (wanted < 4) wanted = 4;
    if (wanted > MAX_WORKERS) wanted = MAX_WORKERS;

    for (int i = 0; i < wanted; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, workerLoop, NULL) != 0) break;
        pthread_detach(thread);
        worker_count++;
    }
    return worker_count;
}

void initDb(void) {
    if (sqlite3_libversion_number() < 3035000) {
        fprintf(stderr, "Потрібен SQLite 3.35 або новіший, а у вас %s. Оновіть libsqlite3-dev.\n", sqlite3_libversion());
        exit(1);
    }
    sqlite3 *db = open_db();
    char *schema = slurp("database/schema.sql", NULL), *seed = slurp("database/seed.sql", NULL);
    if (!db || !schema || !seed) {
        fprintf(stderr, "Не вдалося відкрити БД або database/*.sql — запускайте сервер з кореня проєкту.\n");
        exit(1);
    }
    if (sqlite3_exec(db, "SELECT json_object('a',1)", 0, 0, 0) != SQLITE_OK) {
        fprintf(stderr, "Ваш SQLite зібрано без підтримки JSON — встановіть новіший libsqlite3.\n");
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", 0, 0, 0);
    sqlite3_exec(db, schema, 0, 0, 0);

    char *sql = sqlite3_mprintf("BEGIN; INSERT INTO meta(key,value) VALUES('seeded','1'); %s; COMMIT;", seed);
    if (sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    sqlite3_free(sql);

    const char *email = getenv("ADMIN_EMAIL") ? getenv("ADMIN_EMAIL") : "admin@nexcent.example";
    const char *pass = getenv("ADMIN_PASSWORD") ? getenv("ADMIN_PASSWORD") : "Admin12345!";
    sql = sqlite3_mprintf("INSERT INTO users(name,email,password_hash,role) SELECT 'Administrator',lower(%Q),"
                          "hash_password(%Q),'admin' WHERE NOT EXISTS(SELECT 1 FROM users WHERE role='admin')",
                          email, pass);
    sqlite3_exec(db, sql, 0, 0, 0);
    if (sqlite3_changes(db) > 0) {
        printf("Створено адміністратора: %s\n", email);
        if (!getenv("ADMIN_PASSWORD")) printf("УВАГА: пароль за замовчуванням — задайте ADMIN_PASSWORD перед першим запуском.\n");
    }
    sqlite3_free(sql);
    free(schema);
    free(seed);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    db_path = getenv("DB_PATH") ? getenv("DB_PATH") : "database/nexcent.db";
    int port = getenv("PORT") ? atoi(getenv("PORT")) : 3000;
    started_ms = nowMs();
    initDb();

    int srv = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, WORK_QUEUE_SIZE) != 0) {
        perror("bind/listen");
        close(srv);
        return 1;
    }
    if (startWorkers() == 0) {
        fprintf(stderr, "Не вдалося створити робочі потоки.\n");
        close(srv);
        return 1;
    }
    printf("Nexcent: http://localhost:%d\n", port);
    printf("Робочих потоків: %d, черга: %d\n", worker_count, WORK_QUEUE_SIZE);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t len = sizeof peer;
        int fd = accept(srv, (struct sockaddr *)&peer, &len);
        if (fd < 0) continue;

        req_t *r = calloc(1, sizeof *r);
        if (!r) {
            close(fd);
            continue;
        }
        r->fd = fd;
        inet_ntop(AF_INET, &peer.sin_addr, r->ip, sizeof r->ip);
        if (!queuePush(r)) {
            sendError(r, 503, "Server busy. Try again later.");
            close(fd);
            free(r);
        }
    }
}

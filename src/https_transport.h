/* Local, fixed-target streaming adapter. libcurl supplies verified TLS using
 * the firmware's existing OpenSSL; GStreamer keeps demux, seeking and decoding.
 * No media files, no external process, one bounded worker per active pipeline. */
#include <curl/curl.h>
#include <pthread.h>
static struct {
    int listener, active, started;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_int stop, error, http_status;
    char *url;
} media_proxy = {.listener = -1, .active = -1, .lock = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
    int fd, code, sent, disconnected;
    Buf headers;
} MediaResponse;

static int media_send(MediaResponse *r, const void *data, size_t n) {
    const char *p = data;
    while (n && !atomic_load(&media_proxy.stop)) {
        ssize_t z = send(r->fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (z > 0) {
            p += z;
            n -= (size_t)z;
            continue;
        }
        if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd f = {r->fd, POLLOUT, 0};
            poll(&f, 1, 200);
            if (f.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            continue;
        }
        break;
    }
    if (n)
        r->disconnected = 1;
    return !n;
}
static size_t media_header(char *data, size_t size, size_t count, void *userdata) {
    MediaResponse *r = userdata;
    size_t n = size * count;
    if (n > CAP)
        return 0;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
        clear(&r->headers);
        r->code = 0;
        char line[100];
        size_t len = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, data, len);
        line[len] = 0;
        sscanf(line, "HTTP/%*s %d", &r->code);
    } else if ((n == 2 && !memcmp(data, "\r\n", 2)) || (n == 1 && *data == '\n')) {
        /* Redirect/interim responses are consumed by curl, not the decoder. */
        if (r->code >= 200 && (r->code < 300 || r->code >= 400)) {
            Buf h;
            init(&h);
            fmt(&h, "HTTP/1.1 %d Media\r\nConnection: close\r\n", r->code);
            add(&h, r->headers.s ? r->headers.s : "");
            add(&h, "\r\n");
            int ok = !h.bad && media_send(r, h.s, h.n);
            clear(&h);
            r->sent = ok;
            if (!ok)
                return 0;
            if (r->code >= 400)
                atomic_store(&media_proxy.http_status, r->code);
        }
    } else {
        const char *allowed[] = {
            "Content-Type:", "Content-Length:", "Content-Range:", "Accept-Ranges:", NULL};
        for (int i = 0; allowed[i]; i++)
            if (n >= strlen(allowed[i]) && !strncasecmp(data, allowed[i], strlen(allowed[i]))) {
                addn(&r->headers, data, n);
                if (r->headers.n > 8192 || r->headers.bad)
                    return 0;
                break;
            }
    }
    return n;
}
static size_t media_body(char *data, size_t size, size_t count, void *userdata) {
    MediaResponse *r = userdata;
    size_t n = size * count;
    if (r->code >= 300 && r->code < 400)
        return n;
    return r->sent && media_send(r, data, n) ? n : 0;
}
static int media_progress(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)p;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return atomic_load(&media_proxy.stop);
}
static void *media_worker(void *unused) {
    (void)unused;
    while (!atomic_load(&media_proxy.stop)) {
        struct pollfd f = {media_proxy.listener, POLLIN, 0};
        if (poll(&f, 1, 200) <= 0)
            continue;
        if (atomic_load(&media_proxy.stop))
            break;
        int fd = accept4(media_proxy.listener, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&media_proxy.lock);
        media_proxy.active = fd;
        pthread_mutex_unlock(&media_proxy.lock);
        struct timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char request[8193];
        size_t used = 0;
        int valid = 0;
        while (used < 8192 && !atomic_load(&media_proxy.stop)) {
            ssize_t n = recv(fd, request + used, 8192 - used, 0);
            if (n <= 0)
                break;
            used += (size_t)n;
            request[used] = 0;
            if (strstr(request, "\r\n\r\n")) {
                valid = 1;
                break;
            }
        }
        int head = used >= 5 && !memcmp(request, "HEAD ", 5);
        if (valid && (head || (used >= 4 && !memcmp(request, "GET ", 4)))) {
            char range[128] = "";
            const char *p = request;
            while ((p = strstr(p, "\r\n")) != NULL) {
                p += 2;
                if (strncasecmp(p, "Range:", 6))
                    continue;
                const char *v = p + 6;
                while (*v == ' ' || *v == '\t')
                    v++;
                if (!strncmp(v, "bytes=", 6)) {
                    v += 6;
                    size_t n = strcspn(v, "\r\n");
                    if (n < sizeof(range)) {
                        int ok = n > 0;
                        for (size_t i = 0; i < n; i++)
                            if (!isdigit((unsigned char)v[i]) && v[i] != '-' && v[i] != ',')
                                ok = 0;
                        if (ok) {
                            memcpy(range, v, n);
                            range[n] = 0;
                        }
                    }
                }
                break;
            }
            MediaResponse response = {.fd = fd};
            CURL *c = curl_easy_init();
            if (c) {
                curl_easy_setopt(c, CURLOPT_URL, media_proxy.url);
                curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
                curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,
                                 !strncmp(media_proxy.url, "https://", 8) ? "https" : "http,https");
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
                curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
                curl_easy_setopt(c, CURLOPT_USERAGENT, "StationDLNA/0.1.1");
                curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
                const char *ca = getenv("STATION_CA_BUNDLE");
                if (!ca)
                    ca = "/system/vendor/quasar/ca-certificates.crt";
                curl_easy_setopt(c, CURLOPT_CAINFO, ca);
                curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
                curl_easy_setopt(c, CURLOPT_NOPROXY, "*");
                curl_easy_setopt(c, CURLOPT_NOBODY, head ? 1L : 0L);
                if (*range)
                    curl_easy_setopt(c, CURLOPT_RANGE, range);
                curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, media_header);
                curl_easy_setopt(c, CURLOPT_HEADERDATA, &response);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, media_body);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, media_progress);
                CURLcode code = curl_easy_perform(c);
                if (code != CURLE_OK && !response.disconnected && !atomic_load(&media_proxy.stop))
                    atomic_store(&media_proxy.error, code);
                if (!response.sent && !atomic_load(&media_proxy.stop)) {
                    const char msg[] = "HTTP/1.1 502 Media Error\r\nContent-Length: "
                                       "0\r\nConnection: close\r\n\r\n";
                    media_send(&response, msg, sizeof(msg) - 1);
                }
                curl_easy_cleanup(c);
            }
            clear(&response.headers);
        }
        pthread_mutex_lock(&media_proxy.lock);
        media_proxy.active = -1;
        close(fd);
        pthread_mutex_unlock(&media_proxy.lock);
    }
    return NULL;
}
static void media_proxy_stop(void) {
    if (!media_proxy.started)
        return;
    atomic_store(&media_proxy.stop, 1);
    pthread_mutex_lock(&media_proxy.lock);
    if (media_proxy.active >= 0)
        shutdown(media_proxy.active, SHUT_RDWR);
    pthread_mutex_unlock(&media_proxy.lock);
    shutdown(media_proxy.listener, SHUT_RDWR);
    pthread_join(media_proxy.thread, NULL);
    close(media_proxy.listener);
    media_proxy.listener = -1;
    media_proxy.started = 0;
    free(media_proxy.url);
    media_proxy.url = NULL;
}
static int media_proxy_start(const char *source, char local_url[100]) {
    media_proxy_stop();
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t n = sizeof(address);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &n) < 0) {
        close(fd);
        return 0;
    }
    media_proxy.listener = fd;
    media_proxy.url = strdup(source);
    atomic_store(&media_proxy.stop, 0);
    atomic_store(&media_proxy.error, 0);
    atomic_store(&media_proxy.http_status, 0);
    if (!media_proxy.url || pthread_create(&media_proxy.thread, NULL, media_worker, NULL)) {
        close(fd);
        free(media_proxy.url);
        media_proxy.url = NULL;
        media_proxy.listener = -1;
        return 0;
    }
    media_proxy.started = 1;
    snprintf(local_url, 100, "http://127.0.0.1:%u/media", ntohs(address.sin_port));
    return 1;
}

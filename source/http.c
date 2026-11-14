#include "http.h"
#include "net.h"
#include "instance.h"
#include <dirent.h>
#include <strings.h>\n#include <stdlib.h>

static int send_all(int fd, const void *data, size_t len) {
  const char *p = (const char *)data;
  while (len) {
    int n = sceNetSend(fd, p, (unsigned int)len, 0);
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static void send_error(int fd, int code, const char *reason, const char *body) {
  char h[512];
  int n = snprintf(h, sizeof(h),
    "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: %lu\r\nConnection: close\r\n"
    "X-Content-Type-Options: nosniff\r\n\r\n",
    code, reason, (unsigned long)strlen(body));
  send_all(fd, h, (size_t)n);
  send_all(fd, body, strlen(body));
}

static int url_decode(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '%') {
      if (!r[1] || !r[2]) return -1;
      char hex[3] = {r[1], r[2], 0};
      char *end = NULL;
      long v = strtol(hex, &end, 16);
      if (!end || *end || v == 0) return -1;
      *w++ = (char)v; r += 3;
    } else {
      *w++ = (*r == '+') ? ' ' : *r;
      r++;
    }
  }
  *w = 0;
  return 0;
}

static int safe_relative_path(const char *p) {
  if (!p || *p == '/' || strchr(p, '\\') || strchr(p, ':')) return 0;
  const char *s = p;
  while (*s) {
    const char *e = strchr(s, '/');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
    if (n == 1 && s[0] == '.') return 0;
    s = e ? e + 1 : s + n;
  }
  return 1;
}

static void json_string(char *out, size_t cap, const char *in) {
  size_t used = 0;
  if (cap < 3) { if (cap) out[0] = 0; return; }
  out[used++] = '"';
  while (*in && used + 7 < cap) {
    unsigned char c = (unsigned char)*in++;
    if (c == '"' || c == '\\') { out[used++]='\\'; out[used++]=(char)c; }
    else if (c < 32) { used += (size_t)snprintf(out+used, cap-used, "\\u%04x", c); }
    else out[used++] = (char)c;
  }
  out[used++] = '"'; out[used] = 0;
}

static int inside_root(char *path) {
  char resolved[PATH_MAX];
  if (!realpath(path, resolved)) return 0;
  size_t rootlen = strlen(WEB_ROOT);
  if (strncmp(resolved, WEB_ROOT, rootlen) != 0 ||
      (resolved[rootlen] != '/' && resolved[rootlen] != 0)) return 0;
  snprintf(path, PATH_MAX, "%s", resolved);
  return 1;
}

static int is_video_file(const char *name) {
  const char *e = strrchr(name, '.');
  if (!e) return 0;
  return !strcasecmp(e,".mp4") || !strcasecmp(e,".m4v") ||
         !strcasecmp(e,".mkv") || !strcasecmp(e,".webm") ||
         !strcasecmp(e,".mov") || !strcasecmp(e,".avi") ||
         !strcasecmp(e,".mpg") || !strcasecmp(e,".mpeg") ||
         !strcasecmp(e,".ts") || !strcasecmp(e,".m2ts");
}

static void handle_list(int fd, const char *relative) {
  char dirpath[PATH_MAX];
  if (!safe_relative_path(relative) ||
      snprintf(dirpath, sizeof(dirpath), "%s%s%s", WEB_ROOT,
               *relative ? "/" : "", relative) >= (int)sizeof(dirpath)) {
    send_error(fd, 400, "Bad Request", "Invalid directory"); return;
  }
  if (!inside_root(dirpath)) { send_error(fd, 403, "Forbidden", "Directory outside media root"); return; }
  DIR *d = opendir(dirpath);
  if (!d) { send_error(fd, 404, "Not Found", "Directory not found"); return; }

  char json[65536];
  size_t used = (size_t)snprintf(json, sizeof(json), "{\"path\":");
  char qpath[PATH_MAX * 2];
  json_string(qpath, sizeof(qpath), relative);
  used += (size_t)snprintf(json+used, sizeof(json)-used, "%s,\"items\":[", qpath);
  int first = 1, count = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && count < 500 && used < sizeof(json)-1024) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
        !strcmp(de->d_name, ".instance")) continue;
    char full[PATH_MAX];
    if (snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name) >= (int)sizeof(full)) continue;
    struct stat st;
    if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;
    int isdir = S_ISDIR(st.st_mode);
    if (!isdir && !is_video_file(de->d_name)) continue;
    char qname[1024];
    json_string(qname, sizeof(qname), de->d_name);
    int n = snprintf(json+used, sizeof(json)-used,
      "%s{\"name\":%s,\"dir\":%s,\"size\":%lld}",
      first ? "" : ",", qname, isdir ? "true" : "false",
      (long long)(isdir ? 0 : st.st_size));
    if (n < 0 || (size_t)n >= sizeof(json)-used) break;
    used += (size_t)n; first = 0; count++;
  }
  closedir(d);
  used += (size_t)snprintf(json+used, sizeof(json)-used, "]}");
  char h[256];
  int hn = snprintf(h, sizeof(h),
    "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
    "Content-Length: %lu\r\nCache-Control: no-store\r\n"
    "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
    (unsigned long)used);
  send_all(fd, h, (size_t)hn);
  send_all(fd, json, used);
}

static void serve_file(int fd, const char *path, int head_only, const char *range) {
  struct stat st;
  char resolved[PATH_MAX];
  snprintf(resolved, sizeof(resolved), "%s", path);
  if (!inside_root(resolved)) { send_error(fd, 403, "Forbidden", "File outside media root"); return; }
  int file = open(resolved, O_RDONLY, 0);
  if (file < 0 || fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (file >= 0) close(file);
    send_error(fd, 404, "Not Found", "File not found"); return;
  }

  long long size = (long long)st.st_size, start = 0, end = size ? size-1 : 0;
  int partial = 0;
  if (range && *range) {
    long long a = -1, b = -1;
    if (sscanf(range, "bytes=%lld-%lld", &a, &b) < 1 || size <= 0) {
      close(file); send_error(fd, 416, "Range Not Satisfiable", "Invalid range"); return;
    }
    if (a < 0 && b > 0) { start = size - b; if (start < 0) start = 0; }
    else if (a >= 0) { start = a; if (b >= 0 && b < end) end = b; }
    if (start >= size || end < start) {
      char h[256];
      int n = snprintf(h, sizeof(h),
        "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n", size);
      send_all(fd, h, (size_t)n); close(file); return;
    }
    partial = 1;
  }
  long long length = size ? end-start+1 : 0;
  char h[768];
  int hn = snprintf(h, sizeof(h),
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
    "Content-Length: %lld\r\nAccept-Ranges: bytes\r\n"
    "%s"
    "Cache-Control: no-store\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n\r\n",
    partial ? 206 : 200, partial ? "Partial Content" : "OK",
    get_mime_type(path), length,
    partial ? "" : "");
  if (partial) {
    hn = snprintf(h, sizeof(h),
      "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
      "Content-Length: %lld\r\nAccept-Ranges: bytes\r\n"
      "Content-Range: bytes %lld-%lld/%lld\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
      get_mime_type(path), length, start, end, size);
  }
  if (send_all(fd, h, (size_t)hn) == 0 && !head_only && length > 0) {
    if (lseek(file, (off_t)start, SEEK_SET) >= 0) {
      char buffer[65536];
      long long left = length;
      while (left > 0) {
        size_t want = left > (long long)sizeof(buffer) ? sizeof(buffer) : (size_t)left;
        int got = (int)read(file, buffer, want);
        if (got <= 0 || send_all(fd, buffer, (size_t)got) < 0) break;
        left -= got;
      }
    }
  }
  close(file);
}

void handle_client(int client_fd) {
  char request[BUFFER_SIZE];
  size_t used = 0;
  while (used < sizeof(request)-1) {
    int n = sceNetRecv(client_fd, request+used, (unsigned int)(sizeof(request)-1-used), 0);
    if (n <= 0) return;
    used += (size_t)n; request[used] = 0;
    if (strstr(request, "\r\n\r\n")) break;
  }
  if (!strstr(request, "\r\n\r\n")) { send_error(client_fd, 400, "Bad Request", "Headers too large"); return; }

  int head_only = !strncmp(request, "HEAD ", 5);
  if (strncmp(request, "GET ", 4) && !head_only) {
    send_error(client_fd, 405, "Method Not Allowed", "Method Not Allowed"); return;
  }
  char *request_path = request + (head_only ? 5 : 4);
  char *space = strchr(request_path, ' ');
  if (!space) return;
  *space = 0;
  char decoded_path[PATH_MAX];
  if (strlen(request_path) >= sizeof(decoded_path)) { send_error(client_fd,414,"URI Too Long","Path too long"); return; }
  snprintf(decoded_path, sizeof(decoded_path), "%s", request_path);
  char *path = decoded_path;

  char range[128] = {0};
  char *line = strstr(request, "\r\n");
  while (line && *line) {
    line += 2;
    if (!strncasecmp(line, "Range:", 6)) {
      char *v = line + 6; while (*v == ' ') v++;
      char *e = strstr(v, "\r\n");
      if (e) { size_t n = (size_t)(e-v); if (n >= sizeof(range)) n=sizeof(range)-1; memcpy(range,v,n); range[n]=0; }
      break;
    }
    if (!strncmp(line, "\r\n", 2)) break;
    line = strstr(line, "\r\n");
  }

  char *query = strchr(path, '?');
  if (query) *query = 0;
  if (url_decode(path) < 0) { send_error(client_fd,400,"Bad Request","Bad URL"); return; }

  if (!strcmp(path, "/shutdown")) {
    const char *body = "Shutting down";
    char h[256]; int n=snprintf(h,sizeof(h),"HTTP/1.1 200 OK\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",(unsigned long)strlen(body));
    send_all(client_fd,h,(size_t)n); send_all(client_fd,body,strlen(body));
    printf_notification("Video server shutting down..."); shutdown_server(); return;
  }

  if (!strcmp(path, "/api/list")) {
    char rel[PATH_MAX] = {0};
    if (query) {
      char *p = strstr(query+1, "path=");
      if (p) { p += 5; snprintf(rel,sizeof(rel),"%s",p); if (url_decode(rel)<0) rel[0]=0; }
    }
    handle_list(client_fd, rel); return;
  }

  if (!strcmp(path, "/")) {
    snprintf(path, sizeof(decoded_path), "/index.html");
  }
  if (path[0] != '/' || !safe_relative_path(path+1)) {
    send_error(client_fd,403,"Forbidden","Forbidden"); return;
  }
  char full[PATH_MAX];
  if (snprintf(full,sizeof(full),"%s%s",WEB_ROOT,path) >= (int)sizeof(full)) {
    send_error(client_fd,414,"URI Too Long","Path too long"); return;
  }
  struct stat st;
  if (stat(full,&st)==0 && S_ISDIR(st.st_mode)) {
    size_t n=strlen(full);
    if (n+11 < sizeof(full)) { if (full[n-1]!='/') full[n++]='/'; strcpy(full+n,"index.html"); }
  }
  serve_file(client_fd,full,head_only,range);
}

void *client_thread_entry(void *arg) {
  int client_fd = (int)(intptr_t)arg;
  handle_client(client_fd);
  sceNetSocketClose(client_fd);
  __sync_fetch_and_sub(&g_client_count, 1);
  return NULL;
}

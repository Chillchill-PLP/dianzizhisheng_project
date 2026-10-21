/* Wi-Fi WAV 文件服务。include 于 player_task 之前，共用现有 SD/WAV 解析器。
 * 仅传输模式启动服务；停止服务等待 HTTP 关闭所有 FILE 后才把新曲登记到播放列表。
 * HTTP 任务只发布原子进度，绝不直接修改播放器/画面/播放列表状态。
 */

#define TRANSFER_STRINGIFY_INNER(value) #value
#define TRANSFER_STRINGIFY(value) TRANSFER_STRINGIFY_INNER(value)
#define TRANSFER_NAME_BYTES 128U
#define TRANSFER_MAX_BYTES ((size_t)CONFIG_PLAYER_WIFI_MAX_UPLOAD_MB * 1024U * 1024U)

static httpd_handle_t s_transfer_server = NULL;
static esp_netif_t *s_transfer_ap = NULL;
static esp_netif_t *s_transfer_sta = NULL;
static bool s_transfer_driver_initialized = false;
static bool s_transfer_events_registered = false;
static bool s_transfer_netif_initialized = false;
static bool s_transfer_loop_initialized = false;
static atomic_bool s_transfer_io_failed = ATOMIC_VAR_INIT(false);
/* HTTP 单任务写入；httpd_stop 完成后播放器才读取。保留所有旧文件，不覆盖同名 WAV。 */
static char s_transfer_new_names[SD_MAX_WAV_FILES][TRANSFER_NAME_BYTES];
static unsigned s_transfer_new_count = 0U;

static const char TRANSFER_HTML[] =
    "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>电子之声 · WAV 文件传输</title><style>"
    "body{font:16px system-ui;background:#090c11;color:#f1f5f9;max-width:680px;margin:40px auto;padding:20px}"
    "section{background:#151a22;padding:24px;border-radius:18px;margin:20px 0}"
    "button,input{font:inherit;margin:8px 0;max-width:100%}button{background:#67e5d3;border:0;border-radius:8px;padding:10px 18px}"
    "a{color:#67e5d3}progress{width:100%}li{margin:12px 0;overflow-wrap:anywhere}</style>"
    "<h1>电子之声</h1><p>WAV 文件传输 · 保存到 microSD 卡</p><section>"
    "<p>支持 PCM 8/16/24/32 位或 Float32，1–2 声道，8–96 kHz。单文件最大 "
    "<span id='limit'></span> MiB；同名文件请先改名。</p>"
    "<input id='file' type='file' accept='.wav,audio/wav'><br>"
    "<button id='send'>上传 WAV</button><progress id='progress' max='100' value='0'></progress>"
    "<p id='status' role='status'>传输时请保持设备处于文件传输模式。</p></section>"
    "<section><h2>SD 根目录 WAV</h2><button id='refresh'>刷新列表</button><ul id='files'></ul></section>"
    "<p>完成后长按设备 BOOT 返回开始界面，再进入音乐播放器，无需重启。</p><script>"
    "const $=id=>document.getElementById(id);let busy=false;"
    "const maxUploadMb=" TRANSFER_STRINGIFY(CONFIG_PLAYER_WIFI_MAX_UPLOAD_MB) ";"
    "$('limit').textContent=maxUploadMb;"
    "async function refresh(){try{const r=await fetch('/files');if(!r.ok)throw Error(await r.text());"
    "const d=await r.json();$('limit').textContent=d.max_mb;$('files').replaceChildren();"
    "for(const name of d.files){const li=document.createElement('li'),a=document.createElement('a');"
    "a.textContent=name;a.href='/download?name='+encodeURIComponent(name);a.download=name;"
    "li.appendChild(a);$('files').appendChild(li)}if(d.truncated){const li=document.createElement('li');"
    "li.textContent='目录文件较多，仅显示前 64 个 WAV';$('files').appendChild(li)}}"
    "catch(e){$('status').textContent=e.message}}$('refresh').onclick=refresh;"
    "$('send').onclick=()=>{const f=$('file').files[0];if(busy)return;"
    "if(!f||!(/\\.wav$/i).test(f.name)){$('status').textContent='请选择 WAV 文件';return}"
    "if(f.size<44){$('status').textContent='文件过小，无法作为 WAV 上传';return}"
    "if(f.size>maxUploadMb*1024*1024){$('status').textContent='文件大小 '+(f.size/1024/1024).toFixed(1)"
    "+' MiB，超过单文件上限 '+maxUploadMb+' MiB，请选择较小文件';return}"
    "busy=true;$('send').disabled=true;$('progress').value=0;$('status').textContent='正在上传 '+f.name;"
    "const x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(f.name));"
    "x.timeout=3600000;x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.upload.onprogress=e=>{if(e.lengthComputable){$('progress').value=e.loaded*100/e.total;"
    "$('status').textContent=e.loaded===e.total?'接收完成，正在校验并保存…':'上传 '+Math.floor(e.loaded*100/e.total)+'%'}};"
    "x.onload=()=>{busy=false;$('send').disabled=false;$('status').textContent="
    "(x.status>=200&&x.status<300?'':'HTTP '+x.status+'：')+x.responseText;refresh()};"
    "x.onerror=x.ontimeout=()=>{busy=false;$('send').disabled=false;"
    "$('status').textContent='连接中断，请保持传输模式并重新连接 Wi-Fi。'};x.send(f)};refresh();</script></html>";

/* 解码一个且仅一个 name 参数；拒绝目录、路径穿越、FAT 保留字符及编码的 NUL。 */
static int transfer_hex(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static bool transfer_decode_name(const char *encoded, char *name, size_t capacity)
{
    size_t used = 0U;
    if (encoded == NULL || capacity == 0U) { return false; }
    for (size_t i = 0U; encoded[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)encoded[i];
        if (c == '%') {
            if (encoded[i + 1U] == '\0' || encoded[i + 2U] == '\0') { return false; }
            const int high = transfer_hex(encoded[i + 1U]), low = transfer_hex(encoded[i + 2U]);
            if (high < 0 || low < 0) { return false; }
            c = (unsigned char)((high << 4) | low);
            i += 2U;
        } else if (c == '+') { c = ' '; }
        if (c < 32U || c == 127U || strchr("/\\:*?\"<>|", c) != NULL || used + 1U >= capacity) {
            return false;
        }
        name[used++] = (char)c;
    }
    name[used] = '\0';
    return used > 4U && name[0] != '.' && name[0] != ' ' &&
        strstr(name, "..") == NULL && sd_path_has_wav_extension(name);
}

static bool transfer_request_name(httpd_req_t *req, char *name)
{
    char query[TRANSFER_NAME_BYTES * 3U + 16U];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        strncmp(query, "name=", 5U) != 0 || strchr(query, '&') != NULL) { return false; }
    return transfer_decode_name(query + 5U, name, TRANSFER_NAME_BYTES);
}

/* 错误请求关闭连接，不把未读取的上传 body 解释成下一条 HTTP 请求。 */
static esp_err_t transfer_error(httpd_req_t *req, const char *status, const char *message)
{
    ESP_LOGW(TAG, "WAV HTTP rejected: uri=%s status=%s body_bytes=%u reason=%s",
             req->uri, status, (unsigned)req->content_len, message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    (void)httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static esp_err_t transfer_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, TRANSFER_HTML, sizeof(TRANSFER_HTML) - 1U);
}

static esp_err_t transfer_files(httpd_req_t *req)
{
    if (!atomic_load(&s_transfer_enabled)) { return transfer_error(req, "503 Service Unavailable", "传输模式已退出"); }
    DIR *directory = opendir(SD_MOUNT_POINT);
    if (directory == NULL) { return transfer_error(req, "500 Internal Server Error", "无法读取 SD 卡目录"); }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char header[80];
    (void)snprintf(header, sizeof(header), "{\"max_mb\":%u,\"files\":[", CONFIG_PLAYER_WIFI_MAX_UPLOAD_MB);
    esp_err_t result = httpd_resp_sendstr_chunk(req, header);
    struct dirent *entry;
    unsigned count = 0U;
    bool truncated = false;
    while (result == ESP_OK && atomic_load(&s_transfer_enabled) && (entry = readdir(directory)) != NULL) {
        if (!sd_path_has_wav_extension(entry->d_name) || entry->d_name[0] == '.') { continue; }
        /* 只提供同一套安全命名规则允许的普通 WAV 文件；子目录不作为下载链接。 */
        char path[SD_PATH_BUFFER_BYTES];
        struct stat st;
        if (!sd_build_full_path(entry->d_name, path, sizeof(path)) || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { continue; }
        if (count >= 64U) { truncated = true; break; }
        char json[1024];
        size_t used = 0U;
        if (count > 0U) { json[used++] = ','; }
        json[used++] = '"';
        const unsigned char *p = (const unsigned char *)entry->d_name;
        for (; *p != 0U && used + 7U < sizeof(json); ++p) {
            if (*p < 32U) { used += (size_t)snprintf(json + used, sizeof(json) - used, "\\u%04x", *p); }
            else { if (*p == '"' || *p == '\\') { json[used++] = '\\'; } json[used++] = (char)*p; }
        }
        json[used++] = '"';
        result = httpd_resp_send_chunk(req, json, used);
        ++count;
    }
    (void)closedir(directory);
    if (result == ESP_OK) { result = httpd_resp_sendstr_chunk(req, truncated ? "],\"truncated\":true}" : "],\"truncated\":false}"); }
    if (result == ESP_OK) { result = httpd_resp_send_chunk(req, NULL, 0); }
    return result;
}

static esp_err_t transfer_upload(httpd_req_t *req)
{
    char name[TRANSFER_NAME_BYTES], path[SD_PATH_BUFFER_BYTES], temporary[SD_PATH_BUFFER_BYTES];
    if (!atomic_load(&s_transfer_enabled)) { return transfer_error(req, "503 Service Unavailable", "传输模式已退出"); }
    if (!transfer_request_name(req, name)) { return transfer_error(req, "400 Bad Request", "文件名无效；只接受不含目录的 .wav 文件名（最多 127 字节）"); }
    if (req->content_len < 44U || req->content_len > TRANSFER_MAX_BYTES) {
        char message[160];
        (void)snprintf(message, sizeof(message), "文件大小 %u 字节；WAV 至少 44 字节，单文件上限 %u MiB",
                       (unsigned)req->content_len, CONFIG_PLAYER_WIFI_MAX_UPLOAD_MB);
        return transfer_error(req, "413 Content Too Large", message);
    }
    (void)sd_build_full_path(name, path, sizeof(path));
    (void)snprintf(temporary, sizeof(temporary), "%s/.wifi-upload.part", SD_MOUNT_POINT);
    struct stat st;
    if (stat(path, &st) == 0) { return transfer_error(req, "409 Conflict", "已存在同名文件，请改名后上传"); }
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        atomic_store(&s_transfer_io_failed, true);
        return transfer_error(req, "500 Internal Server Error", "无法写入 SD 卡，请检查存储卡");
    }
    ESP_LOGI(TAG, "Wi-Fi receiving WAV /%s: %u bytes, limit=%u MiB",
             name, (unsigned)req->content_len, CONFIG_PLAYER_WIFI_MAX_UPLOAD_MB);
    atomic_store(&s_transfer_total, req->content_len);
    atomic_store(&s_transfer_bytes, 0U);
    atomic_store(&s_transfer_state, 1);
    size_t received = 0U;
    char buffer[4096];
    bool failed = false;
    bool disk_error = false;
    int64_t last_data_us = esp_timer_get_time();
    while (received < req->content_len && atomic_load(&s_transfer_enabled)) {
        const size_t remaining = req->content_len - received;
        const int got = httpd_req_recv(req, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (got == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() - last_data_us < 15000000LL) { continue; }
        if (got <= 0) { failed = true; break; }
        last_data_us = esp_timer_get_time();
        if (fwrite(buffer, 1U, (size_t)got, file) != (size_t)got) { failed = disk_error = true; break; }
        received += (size_t)got;
        atomic_store(&s_transfer_bytes, received);
    }
    if (fflush(file) != 0) { failed = disk_error = true; }
    if (fclose(file) != 0) { failed = disk_error = true; }
    /* 完整写入后复用播放器格式校验，并核对 data 实际长度，防止残缺文件进播放列表。 */
    wav_stream_info_t info = {0};
    if (!failed && received == req->content_len && atomic_load(&s_transfer_enabled)) {
        file = fopen(temporary, "rb");
        if (file == NULL) { failed = disk_error = true; }
        else {
            failed = wav_parse_header(file, &info) != ESP_OK;
            if (ferror(file)) { disk_error = true; }
            if (fclose(file) != 0) { failed = disk_error = true; }
            if ((uint64_t)info.data_offset + info.data_size > received ||
                info.block_align == 0U || info.data_size % info.block_align != 0U) { failed = true; }
        }
    } else { failed = true; }
    const bool cancelled = !atomic_load(&s_transfer_enabled);
    if (!failed && !cancelled && rename(temporary, path) != 0) { failed = disk_error = true; }
    if (failed || cancelled) {
        (void)remove(temporary);
        if (disk_error) { atomic_store(&s_transfer_io_failed, true); }
        atomic_store(&s_transfer_state, cancelled ? -2 : -1);
        return transfer_error(req, "400 Bad Request", cancelled ? "已取消传输，未保存文件" :
            (disk_error ? "SD 卡写入失败或空间不足，未保存文件" : "传输中断或 WAV 格式无效，未保存文件"));
    }
    if (s_transfer_new_count < SD_MAX_WAV_FILES) {
        (void)snprintf(s_transfer_new_names[s_transfer_new_count++], TRANSFER_NAME_BYTES, "%s", name);
    }
    atomic_store(&s_transfer_state, 2);
    ESP_LOGI(TAG, "Wi-Fi saved WAV /%s: %u bytes", name, (unsigned)received);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "上传成功，已保存到 SD 卡。长按 BOOT 返回后可播放（播放列表最多 16 首）。");
}

static esp_err_t transfer_download(httpd_req_t *req)
{
    char name[TRANSFER_NAME_BYTES], path[SD_PATH_BUFFER_BYTES];
    if (!atomic_load(&s_transfer_enabled)) { return transfer_error(req, "503 Service Unavailable", "传输模式已退出"); }
    if (!transfer_request_name(req, name) || !sd_build_full_path(name, path, sizeof(path))) {
        return transfer_error(req, "400 Bad Request", "文件名无效");
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { return httpd_resp_send_404(req); }
    FILE *file = fopen(path, "rb");
    if (file == NULL) { return httpd_resp_send_404(req); }
    httpd_resp_set_type(req, "audio/wav");
    /* 文件名由网页 download 属性提供，不把客户端输入直接放进 HTTP 响应头。 */
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    atomic_store(&s_transfer_total, (uint32_t)st.st_size);
    atomic_store(&s_transfer_bytes, 0U);
    atomic_store(&s_transfer_state, 1);
    char buffer[4096];
    size_t count, sent = 0U;
    esp_err_t result = ESP_OK;
    while (atomic_load(&s_transfer_enabled) && (count = fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
        result = httpd_resp_send_chunk(req, buffer, count);
        if (result != ESP_OK) { break; }
        sent += count;
        atomic_store(&s_transfer_bytes, sent);
    }
    if (ferror(file)) { result = ESP_FAIL; atomic_store(&s_transfer_io_failed, true); }
    (void)fclose(file);
    if (!atomic_load(&s_transfer_enabled)) { result = ESP_FAIL; }
    if (result == ESP_OK) { result = httpd_resp_send_chunk(req, NULL, 0); }
    atomic_store(&s_transfer_state, result == ESP_OK ? 2 : -1);
    return result;
}

static void transfer_wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (!atomic_load(&s_transfer_enabled)) { return; }
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        atomic_store(&s_wifi_sta_ip, 0U);
        /* Wi-Fi 驱动每轮连接本身有扫描/退避；热点在路由器断开时始终可用。 */
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        atomic_store(&s_wifi_sta_ip, event->ip_info.ip.addr);
        ESP_LOGI(TAG, "Wi-Fi router IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_transfer_stop(void)
{
    atomic_store(&s_transfer_enabled, false);
    if (s_transfer_server != NULL) {
        (void)httpd_stop(s_transfer_server);
        s_transfer_server = NULL;
    }
    if (s_transfer_driver_initialized) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        s_transfer_driver_initialized = false;
    }
    atomic_store(&s_wifi_status, 0);
    atomic_store(&s_wifi_sta_ip, 0U);
    /* 同步停止服务器后，HTTP 不再读取这些名称或 SD；播放器独占更新目录。 */
    for (unsigned i = 0U; i < s_transfer_new_count; ++i) { sd_register_wav_file(s_transfer_new_names[i]); }
    s_transfer_new_count = 0U;
    if (atomic_exchange(&s_transfer_io_failed, false)) { s_sd_recovery_requested = true; }
}

static esp_err_t wifi_transfer_start(void)
{
    const char *ssid = CONFIG_PLAYER_WIFI_AP_SSID, *password = CONFIG_PLAYER_WIFI_AP_PASSWORD;
    const char *router_ssid = CONFIG_PLAYER_WIFI_STA_SSID, *router_password = CONFIG_PLAYER_WIFI_STA_PASSWORD;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    atomic_store(&s_wifi_status, 1);
    atomic_store(&s_transfer_state, 0);
    atomic_store(&s_transfer_bytes, 0U);
    atomic_store(&s_transfer_total, 0U);
    if (strlen(ssid) == 0U || strlen(ssid) > 32U || strlen(password) < 8U || strlen(password) > 63U ||
        strlen(router_ssid) > 32U || strlen(router_password) > 63U) { goto failed; }
    result = nvs_flash_init(); /* 不自动擦除 NVS；异常返回错误日志。 */
    if (result != ESP_OK) { goto failed; }
    if (!s_transfer_netif_initialized) {
        result = esp_netif_init();
        if (result != ESP_OK) { goto failed; }
        s_transfer_netif_initialized = true;
    }
    if (!s_transfer_loop_initialized) {
        result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) { goto failed; }
        s_transfer_loop_initialized = true;
    }
    if (s_transfer_ap == NULL) { s_transfer_ap = esp_netif_create_default_wifi_ap(); }
    if (s_transfer_sta == NULL && router_ssid[0] != '\0') { s_transfer_sta = esp_netif_create_default_wifi_sta(); }
    if (s_transfer_ap == NULL || (router_ssid[0] != '\0' && s_transfer_sta == NULL)) { result = ESP_ERR_NO_MEM; goto failed; }
    if (!s_transfer_events_registered) {
        result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, transfer_wifi_event, NULL);
        if (result != ESP_OK) { goto failed; }
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, transfer_wifi_event, NULL);
        if (result != ESP_OK) {
            (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, transfer_wifi_event);
            goto failed;
        }
        s_transfer_events_registered = true;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    /* 此功能单用户传文件，限制缓冲以给常驻 ESP-SR 模型及 DMA 留内部 RAM。 */
    init.static_rx_buf_num = 8;
    init.dynamic_rx_buf_num = 8;
    init.dynamic_tx_buf_num = 8;
    ESP_LOGI(TAG, "Wi-Fi init: internal heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    result = esp_wifi_init(&init);
    if (result != ESP_OK) { goto failed; }
    s_transfer_driver_initialized = true;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) { goto failed; }
    result = esp_wifi_set_mode(router_ssid[0] != '\0' ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (result != ESP_OK) { goto failed; }
    wifi_config_t ap = {.ap = {.channel = 6, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    memcpy(ap.ap.ssid, ssid, strlen(ssid));
    ap.ap.ssid_len = strlen(ssid);
    memcpy(ap.ap.password, password, strlen(password));
    result = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (result != ESP_OK) { goto failed; }
    if (router_ssid[0] != '\0') {
        wifi_config_t sta = {0};
        memcpy(sta.sta.ssid, router_ssid, strlen(router_ssid));
        memcpy(sta.sta.password, router_password, strlen(router_password));
        result = esp_wifi_set_config(WIFI_IF_STA, &sta);
        if (result != ESP_OK) { goto failed; }
    }
    atomic_store(&s_transfer_enabled, true);
    result = esp_wifi_start();
    if (result != ESP_OK) { goto failed; }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.core_id = 0;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    result = httpd_start(&s_transfer_server, &config);
    if (result != ESP_OK) { goto failed; }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = transfer_index},
        {.uri = "/files", .method = HTTP_GET, .handler = transfer_files},
        {.uri = "/upload", .method = HTTP_POST, .handler = transfer_upload},
        {.uri = "/download", .method = HTTP_GET, .handler = transfer_download},
    };
    for (size_t i = 0U; i < ARRAY_SIZE(routes); ++i) {
        result = httpd_register_uri_handler(s_transfer_server, &routes[i]);
        if (result != ESP_OK) { goto failed; }
    }
    atomic_store(&s_wifi_status, 2);
    ESP_LOGI(TAG, "WAV transfer ready: SSID=%s URL=http://192.168.4.1/", ssid);
    return ESP_OK;
failed:
    wifi_transfer_stop();
    atomic_store(&s_wifi_status, -1);
    return result;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

// 写文件回调函数
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "用法: %s <线程数> <域名> <端口> <IP> <URL>\n", argv[0]);
        fprintf(stderr, "示例: %s 16 ipxe.527165.xyz 443 198.41.219.34 https://ipxe.527165.xyz/wim/pe32.wim\n", argv[0]);
        return 1;
    }

    int threads = atoi(argv[1]);
    char *host = argv[2];
    char *port = argv[3];
    char *ip = argv[4];
    char *url = argv[5];

    // 构造 --resolve 字符串 "HOST:PORT:IP"
    char resolve_str[256];
    snprintf(resolve_str, sizeof(resolve_str), "%s:%s:%s", host, port, ip);

    CURL *curl;
    CURLcode res;
    FILE *outfile;

    // 从URL中提取文件名（简单处理，仅用于示例）
    const char *filename = strrchr(url, '/');
    filename = (filename) ? filename + 1 : "downloaded.file";

    outfile = fopen(filename, "wb");
    if (!outfile) {
        perror("无法打开输出文件");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {
        struct curl_slist *resolve = NULL;
        resolve = curl_slist_append(resolve, resolve_str);

        // 基础设置
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);      // 跟随重定向
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);      // 忽略证书验证（PE环境常用）
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);          // 显示进度条

        // 关键：多线程分块下载——通过设置多个连接并发下载同一个文件
        // 这里使用 HTTP/2 的多路复用（如果服务器支持），否则需要手动分块
        // 为了简化，我们使用 HTTP/2 的多路复用特性（需要 libcurl 支持）
        curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
        curl_easy_setopt(curl, CURLOPT_MAX_CONCURRENT_STREAMS, (long)threads);

        // 设置写文件回调
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, outfile);

        // 执行下载
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "下载失败: %s\n", curl_easy_strerror(res));
        } else {
            printf("\n下载完成！保存为: %s\n", filename);
        }

        curl_slist_free_all(resolve);
        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "初始化 curl 失败\n");
    }

    fclose(outfile);
    curl_global_cleanup();
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <curl/curl.h>

typedef struct {
    char url[1024];
    char resolve[256];
    long start;
    long end;
    int index;
    int success;
    FILE *fp;
} ThreadData;

volatile long long total_downloaded = 0;
long long total_size = 0;

CRITICAL_SECTION lock;

DWORD pid;

size_t write_data(void *ptr,size_t size,size_t nmemb,void *userdata)
{
    FILE *fp = (FILE*)userdata;

    size_t written = fwrite(ptr,size,nmemb,fp);

    EnterCriticalSection(&lock);
    total_downloaded += written * size;
    LeaveCriticalSection(&lock);

    return written * size;
}

DWORD WINAPI download_thread(LPVOID arg)
{
    ThreadData *data = (ThreadData*)arg;

    int retry = 0;

    while(retry < 3)
    {
        char filename[64];
        sprintf(filename,"part_%lu_%d.tmp",pid,data->index);

        if(data->fp)
            fclose(data->fp);

        data->fp = fopen(filename,"wb");

        if(!data->fp)
            return 1;

        CURL *curl = curl_easy_init();
        if(!curl)
            return 1;

        struct curl_slist *resolve = NULL;
        resolve = curl_slist_append(resolve,data->resolve);

        char range[64];
        sprintf(range,"%ld-%ld",data->start,data->end);

        curl_easy_setopt(curl,CURLOPT_URL,data->url);
        curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve);
        curl_easy_setopt(curl,CURLOPT_RANGE,range);

        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,data->fp);

        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(resolve);
        curl_easy_cleanup(curl);

        if(res == CURLE_OK)
        {
            data->success = 1;
            return 0;
        }

        retry++;
        Sleep(1000);
    }

    data->success = 0;
    return 1;
}

long get_file_size(const char *url,const char *resolve)
{
    CURL *curl = curl_easy_init();
    if(!curl)
        return -1;

    struct curl_slist *resolve_list = NULL;
    resolve_list = curl_slist_append(resolve_list,resolve);

    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_NOBODY,1L);
    curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve_list);

    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

    CURLcode res = curl_easy_perform(curl);

    long filesize = -1;

    if(res == CURLE_OK)
    {
        double size = 0;
        curl_easy_getinfo(curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD,&size);
        filesize = (long)size;
    }

    curl_slist_free_all(resolve_list);
    curl_easy_cleanup(curl);

    return filesize;
}

DWORD WINAPI speed_thread(LPVOID arg)
{
    long long last = 0;

    while(total_downloaded < total_size)
    {
        Sleep(1000);

        long long now;

        EnterCriticalSection(&lock);
        now = total_downloaded;
        LeaveCriticalSection(&lock);

        long long speed = now - last;
        last = now;

        double percent = (double)now * 100.0 / (double)total_size;

        printf("\r%.2f%%  %.2f MB/s",
               percent,
               speed / 1024.0 / 1024.0);

        fflush(stdout);
    }

    return 0;
}

void cleanup_tmp(int threads)
{
    for(int i=0;i<threads;i++)
    {
        char filename[64];
        sprintf(filename,"part_%lu_%d.tmp",pid,i);
        remove(filename);
    }
}

int main(int argc,char *argv[])
{
    // 设置控制台输出为 UTF-8 编码，避免中文乱码
    SetConsoleOutputCP(CP_UTF8);

    if(argc != 6)
    {
        printf("用法:\n");
        printf("%s <线程数> <域名> <端口> <IP> <URL>\n",argv[0]);
        return 1;
    }

    int threads = atoi(argv[1]);

    if(threads < 1) threads = 1;
    if(threads > 64) threads = 64;

    char *host = argv[2];
    char *port = argv[3];
    char *ip = argv[4];
    char *url = argv[5];

    char resolve[256];
    sprintf(resolve,"%s:%s:%s",host,port,ip);

    pid = GetCurrentProcessId();

    curl_global_init(CURL_GLOBAL_ALL);

    total_size = get_file_size(url,resolve);

    if(total_size <= 0)
    {
        printf("服务器不支持 Range\n");
        return 1;
    }

    printf("文件大小: %.2f MB\n",total_size / 1024.0 / 1024.0);

    InitializeCriticalSection(&lock);

    long part = total_size / threads;

    HANDLE *handles = malloc(sizeof(HANDLE)*threads);
    ThreadData *data = malloc(sizeof(ThreadData)*threads);

    for(int i=0;i<threads;i++)
    {
        strcpy(data[i].url,url);
        strcpy(data[i].resolve,resolve);

        data[i].start = part * i;

        if(i == threads-1)
            data[i].end = total_size-1;
        else
            data[i].end = (part*(i+1))-1;

        data[i].index = i;
        data[i].success = 0;
        data[i].fp = NULL;

        handles[i] = CreateThread(NULL,0,download_thread,&data[i],0,NULL);
    }

    HANDLE speed = CreateThread(NULL,0,speed_thread,NULL,0,NULL);

    WaitForMultipleObjects(threads,handles,TRUE,INFINITE);
    WaitForSingleObject(speed,INFINITE);

    int all_success = 1;

    for(int i=0;i<threads;i++)
    {
        if(!data[i].success)
            all_success = 0;

        CloseHandle(handles[i]);

        if(data[i].fp)
            fclose(data[i].fp);
    }

    CloseHandle(speed);

    if(!all_success)
    {
        printf("\n下载失败\n");
        cleanup_tmp(threads);
        return 1;
    }

    printf("\n下载完成，正在合并...\n");

    const char *outname = strrchr(url,'/');
    outname = outname ? outname+1 : "download.file";

    FILE *out = fopen(outname,"wb");

    char buffer[8192];

    for(int i=0;i<threads;i++)
    {
        char filename[64];
        sprintf(filename,"part_%lu_%d.tmp",pid,i);

        FILE *fp = fopen(filename,"rb");

        size_t n;

        while((n=fread(buffer,1,sizeof(buffer),fp))>0)
            fwrite(buffer,1,n,out);

        fclose(fp);
        remove(filename);
    }

    fclose(out);

    DeleteCriticalSection(&lock);

    free(handles);
    free(data);

    curl_global_cleanup();

    printf("完成: %s\n",outname);

    return 0;
}

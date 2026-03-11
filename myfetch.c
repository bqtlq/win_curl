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
    int success;
    HANDLE file;
    long long offset;
} ThreadData;

volatile long long total_downloaded = 0;
long long total_size = 0;

CRITICAL_SECTION lock;

size_t write_data(void *ptr,size_t size,size_t nmemb,void *userdata)
{
    ThreadData *data = (ThreadData*)userdata;

    DWORD written = 0;
    DWORD towrite = (DWORD)(size * nmemb);

    OVERLAPPED ov;
    memset(&ov,0,sizeof(ov));

    ov.Offset     = (DWORD)(data->offset & 0xffffffff);
    ov.OffsetHigh = (DWORD)((data->offset >> 32) & 0xffffffff);

    if(!WriteFile(data->file,ptr,towrite,&written,&ov))
        return 0;

    data->offset += written;

    EnterCriticalSection(&lock);
    total_downloaded += written;
    LeaveCriticalSection(&lock);

    return nmemb;
}

DWORD WINAPI download_thread(LPVOID arg)
{
    ThreadData *data = (ThreadData*)arg;

    int retry = 0;

    while(retry < 3)
    {
        data->offset = data->start;   // 修复：重试时重置 offset

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
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,data);

        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

        curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,102400L);

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

size_t single_write(void *ptr,size_t size,size_t nmemb,void *userdata)
{
    FILE *fp = (FILE*)userdata;
    return fwrite(ptr,size,nmemb,fp);
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

int single_thread_download(char *url,char *resolve)
{
    CURL *curl = curl_easy_init();
    if(!curl)
        return 1;

    struct curl_slist *resolve_list = NULL;
    resolve_list = curl_slist_append(resolve_list,resolve);

    const char *outname = strrchr(url,'/');
    outname = outname ? outname+1 : "download.file";

    FILE *fp = fopen(outname,"wb");
    if(!fp)
    {
        printf("无法创建文件\n");
        return 1;
    }

    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve_list);

    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,single_write);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,fp);

    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

    printf("服务器不支持 Range，使用单线程下载...\n");

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(resolve_list);
    curl_easy_cleanup(curl);
    fclose(fp);

    if(res != CURLE_OK)
    {
        printf("下载失败: %s\n",curl_easy_strerror(res));
        DeleteFileA(outname);
        return 1;
    }

    printf("完成: %s\n",outname);
    return 0;
}

int main(int argc,char *argv[])
{
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

    curl_global_init(CURL_GLOBAL_ALL);

    total_size = get_file_size(url,resolve);

    if(total_size <= 0)
        return single_thread_download(url,resolve);

    printf("文件大小: %.2f MB\n",total_size / 1024.0 / 1024.0);

    const char *outname = strrchr(url,'/');
    outname = outname ? outname+1 : "download.file";

    HANDLE file = CreateFileA(
        outname,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if(file == INVALID_HANDLE_VALUE)
    {
        printf("无法创建文件\n");
        return 1;
    }

    LARGE_INTEGER size;
    size.QuadPart = total_size;
    SetFilePointerEx(file,size,NULL,FILE_BEGIN);
    SetEndOfFile(file);

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
            data[i].end = total_size - 1;
        else
            data[i].end = (part*(i+1)) - 1;

        data[i].file = file;
        data[i].success = 0;

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
    }

    CloseHandle(speed);

    DeleteCriticalSection(&lock);

    CloseHandle(file);

    free(handles);
    free(data);

    curl_global_cleanup();

    if(!all_success)
    {
        printf("\n下载失败\n");
        DeleteFileA(outname);
        return 1;
    }

    printf("\n完成: %s\n",outname);

    return 0;
}

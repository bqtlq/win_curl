#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <curl/curl.h>

typedef struct {
    long long offset;
    HANDLE file;
} WriteCtx;

typedef struct {
    char url[1024];
    char resolve[256];
    HANDLE file;
    int success;
} ThreadData;

volatile long long total_downloaded = 0;
long long total_size = 0;

long long next_chunk = 0;
long long chunk_size = 4LL * 1024 * 1024;

volatile int global_failed = 0;

CRITICAL_SECTION lock;

size_t write_data(void *ptr,size_t size,size_t nmemb,void *userdata)
{
    WriteCtx *ctx = (WriteCtx*)userdata;

    DWORD written = 0;
    DWORD towrite = (DWORD)(size * nmemb);

    OVERLAPPED ov;
    memset(&ov,0,sizeof(ov));

    ov.Offset     = (DWORD)(ctx->offset & 0xffffffff);
    ov.OffsetHigh = (DWORD)((ctx->offset >> 32) & 0xffffffff);

    if(!WriteFile(ctx->file,ptr,towrite,&written,&ov))
        return 0;

    ctx->offset += written;

    EnterCriticalSection(&lock);
    total_downloaded += written;
    LeaveCriticalSection(&lock);

    return nmemb;
}

DWORD WINAPI download_thread(LPVOID arg)
{
    ThreadData *data = (ThreadData*)arg;

    while(1)
    {
        if(global_failed)
        {
            data->success = 0;
            return 1;
        }

        long long start;

        EnterCriticalSection(&lock);
        start = next_chunk;
        next_chunk += chunk_size;
        LeaveCriticalSection(&lock);

        if(start >= total_size)
            break;

        long long end = start + chunk_size - 1;
        if(end >= total_size)
            end = total_size - 1;

        int retry = 0;

        while(retry < 3)
        {
            if(global_failed)
            {
                data->success = 0;
                return 1;
            }

            CURL *curl = curl_easy_init();
            if(!curl)
                break;

            struct curl_slist *resolve = NULL;
            resolve = curl_slist_append(resolve,data->resolve);

            char range[128];
            sprintf(range,"%lld-%lld",start,end);

            WriteCtx ctx;
            ctx.offset = start;
            ctx.file   = data->file;

            curl_easy_setopt(curl,CURLOPT_URL,data->url);
            curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve);
            curl_easy_setopt(curl,CURLOPT_RANGE,range);

            curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);
            curl_easy_setopt(curl,CURLOPT_WRITEDATA,&ctx);

            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
            curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

            curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,1024*1024L);
            curl_easy_setopt(curl,CURLOPT_TCP_NODELAY,1L);

            CURLcode res = curl_easy_perform(curl);

            curl_slist_free_all(resolve);
            curl_easy_cleanup(curl);

            if(res == CURLE_OK)
                break;

            retry++;
            Sleep(1000);
        }

        if(retry == 3)
        {
            global_failed = 1;
            data->success = 0;
            return 1;
        }
    }

    data->success = 1;
    return 0;
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

int check_range_support(const char *url,const char *resolve)
{
    CURL *curl = curl_easy_init();
    if(!curl)
        return 0;

    struct curl_slist *resolve_list = NULL;
    resolve_list = curl_slist_append(resolve_list,resolve);

    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_RANGE,"0-0");
    curl_easy_setopt(curl,CURLOPT_NOBODY,1L);
    curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve_list);

    long code = 0;

    if(curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);

    curl_slist_free_all(resolve_list);
    curl_easy_cleanup(curl);

    return (code == 206);
}

DWORD WINAPI speed_thread(LPVOID arg)
{
    long long last = 0;

    while(total_downloaded < total_size && !global_failed)
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

void extract_filename(const char *url,char *name)
{
    const char *p = strrchr(url,'/');
    p = p ? p+1 : "download.file";

    strncpy(name,p,511);
    name[511] = 0;

    char *q = strchr(name,'?');
    if(q) *q = 0;
}

size_t single_write(void *ptr,size_t size,size_t nmemb,void *userdata)
{
    FILE *fp = (FILE*)userdata;
    return fwrite(ptr,size,nmemb,fp);
}

int single_thread_download(char *url,char *resolve,char *filename)
{
    CURL *curl = curl_easy_init();
    if(!curl)
        return 1;

    struct curl_slist *resolve_list = NULL;
    resolve_list = curl_slist_append(resolve_list,resolve);

    FILE *fp = fopen(filename,"wb");
    if(!fp)
        return 1;

    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_RESOLVE,resolve_list);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,single_write);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,fp);

    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(resolve_list);
    curl_easy_cleanup(curl);
    fclose(fp);

    return res != CURLE_OK;
}

int main(int argc,char *argv[])
{
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
    char *ip   = argv[4];
    char *url  = argv[5];

    char resolve[256];
    sprintf(resolve,"%s:%s:%s",host,port,ip);

    curl_global_init(CURL_GLOBAL_ALL);

    total_size = get_file_size(url,resolve);

    if(total_size <= 0)
    {
        printf("无法获取文件大小\n");
        return 1;
    }

    char filename[512];
    extract_filename(url,filename);

    if(!check_range_support(url,resolve))
    {
        printf("服务器不支持 Range，使用单线程下载\n");
        return single_thread_download(url,resolve,filename);
    }

    printf("文件大小: %.2f MB\n",total_size/1024.0/1024.0);

    char tempname[512];
    sprintf(tempname,"%s.part",filename);

    HANDLE file = CreateFileA(
        tempname,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    LARGE_INTEGER size;
    size.QuadPart = total_size;
    SetFilePointerEx(file,size,NULL,FILE_BEGIN);
    SetEndOfFile(file);

    InitializeCriticalSection(&lock);

    HANDLE *handles = malloc(sizeof(HANDLE)*threads);
    ThreadData *data = malloc(sizeof(ThreadData)*threads);

    for(int i=0;i<threads;i++)
    {
        strcpy(data[i].url,url);
        strcpy(data[i].resolve,resolve);
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

    if(total_downloaded != total_size)
        all_success = 0;

    if(!all_success)
    {
        printf("\n下载失败\n");
        DeleteFileA(tempname);
        return 1;
    }

    MoveFileExA(tempname,filename,MOVEFILE_REPLACE_EXISTING);

    printf("\n完成: %s\n",filename);

    return 0;
}

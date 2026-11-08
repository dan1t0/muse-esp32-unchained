/* Copyright (c) 2026 Dani Martinez - dan1t0
 * SPDX-License-Identifier: Apache-2.0 */
#include "tts_host.h"
#include "cJSON.h"
#include "muse_tts.c"

struct queue { pthread_mutex_t lock; pthread_cond_t ready; size_t size; bool full; char data[64]; };
void *heap_caps_malloc(size_t size, int caps) { return malloc(size); }
void heap_caps_free(void *p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { pthread_mutex_t *m=malloc(sizeof(*m));assert(!pthread_mutex_init(m,NULL));return m; }
int xSemaphoreTake(SemaphoreHandle_t m,uint32_t wait) { return !pthread_mutex_lock(m); }
void xSemaphoreGive(SemaphoreHandle_t m) { assert(!pthread_mutex_unlock(m)); }
void vSemaphoreDelete(SemaphoreHandle_t m) { pthread_mutex_destroy(m);free(m); }
QueueHandle_t xQueueCreateWithCaps(int count,size_t size,int caps) {
    assert(count==1&&size<=64);struct queue *q=calloc(1,sizeof(*q));q->size=size;
    pthread_mutex_init(&q->lock,NULL);pthread_cond_init(&q->ready,NULL);return q;
}
int xQueueReceive(QueueHandle_t q,void *out,uint32_t wait) {
    pthread_mutex_lock(&q->lock);
    while(!q->full&&wait) pthread_cond_wait(&q->ready,&q->lock);
    bool full=q->full;if(full){memcpy(out,q->data,q->size);q->full=false;}
    pthread_mutex_unlock(&q->lock);return full;
}
int xQueueSend(QueueHandle_t q,const void *data,uint32_t wait) {
    pthread_mutex_lock(&q->lock);bool ok=!q->full;
    if(ok){memcpy(q->data,data,q->size);q->full=true;pthread_cond_signal(&q->ready);}
    pthread_mutex_unlock(&q->lock);return ok;
}
void vQueueDeleteWithCaps(QueueHandle_t q) { free(q); }
static void (*task_fn)(void *);
static void *task_entry(void *arg) { task_fn(arg);return NULL; }
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *handle,int core,int caps) {
    task_fn=fn;pthread_t t;assert(!pthread_create(&t,NULL,task_entry,arg));pthread_detach(t);return pdPASS;
}
void vTaskDelay(uint32_t ms) { usleep(ms*1000); }
int64_t esp_timer_get_time(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }

struct http { esp_http_client_config_t cfg; int fd[2],status; size_t used,total,at; char body[8192],text[2048]; bool truncated; };
static atomic_int headers_seen, bytes_read;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(!strcmp(cfg->url,"http://tts.test/v1/audio/speech"));
    struct http *h=calloc(1,sizeof(*h));h->cfg=*cfg;h->status=200;h->total=10000;assert(!socketpair(AF_UNIX,SOCK_STREAM,0,h->fd));return h;
}
int esp_http_client_set_header(esp_http_client_handle_t h,const char *key,const char *value) { return ESP_OK; }
int esp_http_client_open(esp_http_client_handle_t h,int len) { return ESP_OK; }
int esp_http_client_get_socket(esp_http_client_handle_t h) { return h->fd[0]; }
int esp_http_client_write(esp_http_client_handle_t h,const char *buf,int len) {
    if(len>47)len=47;assert(h->used+(size_t)len<sizeof(h->body));memcpy(h->body+h->used,buf,len);h->used+=len;return len;
}
int esp_http_client_set_timeout_ms(esp_http_client_handle_t h,int ms) { return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) {
    cJSON *body=cJSON_Parse(h->body);assert(body);
#if CONFIG_MUSE_TTS_STREAM
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"model")),"mlx-community/Kokoro-82M-bf16"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"voice")),"em_santa"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"lang_code")),"e"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(body,"stream")));
    assert(cJSON_GetArraySize(body)==6);
#else
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"model")),"tts-1"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"voice")),"Federico"));
    assert(!cJSON_HasObjectItem(body,"lang_code"));
    assert(!cJSON_HasObjectItem(body,"stream"));
    assert(cJSON_GetArraySize(body)==4);
#endif
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body,"response_format")),"mp3"));
    snprintf(h->text,sizeof(h->text),"%s",cJSON_GetStringValue(cJSON_GetObjectItem(body,"input")));cJSON_Delete(body);
    atomic_fetch_add(&headers_seen,1);
    if(!strcmp(h->text,"slow")){char b;assert(recv(h->fd[0],&b,1,0)==0);return -1;}
    if(!strcmp(h->text,"large"))h->total=200000;
    if(!strcmp(h->text,"oversized"))h->total=MAX_AUDIO_BYTES+1;
    if(!strcmp(h->text,"empty"))h->total=0;
    if(!strcmp(h->text,"error"))h->status=503;
    if(!strcmp(h->text,"truncated")){h->total=256;h->truncated=true;}
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=h->cfg.user_data,
        .header_key="Content-Type",.header_value=!strcmp(h->text,"badtype")?"audio/wav":"audio/mpeg; charset=binary"};
    bool chunked=!strcmp(h->text,"chunked");
    if(chunked)e.header_value="audio/mp3";
    h->cfg.event_handler(&e);return chunked?0:(int64_t)h->total;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
int esp_http_client_read(esp_http_client_handle_t h,char *buf,int cap) {
    size_t n=h->total-h->at;if(n>137)n=137;if(n>(size_t)cap)n=cap;
    for(size_t i=0;i<n;i++)buf[i]=(char)((h->at+i)%251);
    h->at+=n;atomic_fetch_add(&bytes_read,(int)n);return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { return h->at==h->total&&!h->truncated; }
int esp_http_client_cleanup(esp_http_client_handle_t h) { close(h->fd[0]);close(h->fd[1]);free(h);return ESP_OK; }

static void wait_headers(int expected) {
    int64_t until=esp_timer_get_time()+1000000;
    while(atomic_load(&headers_seen)<expected&&esp_timer_get_time()<until)usleep(1000);
    assert(atomic_load(&headers_seen)>=expected);
}
static void collect(uint32_t id,size_t expected,muse_tts_status_t terminal) {
    unsigned char data[313];size_t received=0;int64_t until=esp_timer_get_time()+2000000;
    muse_tts_status_t status=MUSE_TTS_PENDING;
    while(status==MUSE_TTS_PENDING&&esp_timer_get_time()<until) {
        size_t n=muse_tts_read(id,data,sizeof(data),&status);
        for(size_t i=0;i<n;i++)assert(data[i]==(received+i)%251);
        received+=n;if(!n)usleep(1000);
    }
    assert(status==terminal);assert(received==expected);
}
int main(int argc,char **argv) {
    assert(argc==2);assert(muse_tts_init());
    if(!strcmp(argv[1],"normal"))collect(muse_tts_start("Hello \"Muse\"\nEspañol \\"),10000,MUSE_TTS_END);
    else if(!strcmp(argv[1],"chunked"))collect(muse_tts_start("chunked"),10000,MUSE_TTS_END);
    else if(!strcmp(argv[1],"cancel")) {
        uint32_t old=muse_tts_start("slow");wait_headers(1);
        int64_t t=esp_timer_get_time();muse_tts_cancel();
        collect(muse_tts_start("next"),10000,MUSE_TTS_END);
        assert(esp_timer_get_time()-t<1000000);collect(old,0,MUSE_TTS_FAILED);
    } else if(!strcmp(argv[1],"backpressure")) {
        uint32_t old=muse_tts_start("large");wait_headers(1);usleep(100000);
        int bytes=atomic_load(&bytes_read);assert(bytes>=BUFFER_BYTES&&bytes<=BUFFER_BYTES+READ_BYTES);
        usleep(30000);assert(atomic_load(&bytes_read)==bytes);
        collect(muse_tts_start("next"),10000,MUSE_TTS_END);collect(old,0,MUSE_TTS_FAILED);
    } else if(!strcmp(argv[1],"errors")) {
        const char *cases[]={"badtype","oversized","empty","error"};
        for(size_t i=0;i<4;i++)collect(muse_tts_start(cases[i]),0,MUSE_TTS_FAILED);
    } else if(!strcmp(argv[1],"truncated"))collect(muse_tts_start("truncated"),256,MUSE_TTS_FAILED);
    else if(!strcmp(argv[1],"stale")) {
        uint32_t a=muse_tts_start("normal");collect(a,10000,MUSE_TTS_END);
        uint32_t b=muse_tts_start("normal");assert(a!=b);collect(a,0,MUSE_TTS_FAILED);collect(b,10000,MUSE_TTS_END);
    } else return 2;
    muse_tts_cancel();puts("ok");return 0;
}

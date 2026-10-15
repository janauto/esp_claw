#include "clip_feed.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FEED_BYTES_MAX 60000
#define FEED_CARDS_MAX 20
typedef struct { char *text; uint64_t received_ms,initial_age_ms; bool fresh; } feed_slot_t;
struct clip_feed_t { char *root; feed_slot_t slots[3]; };
static const char *const ids[]={"weather","stocks","aihot"};
static int index_of(const char *id) { for (int i=0;i<3;i++) if (id&&!strcmp(id,ids[i])) return i; return -1; }
bool clip_feed_id_valid(const char *id) { return index_of(id)>=0; }
static char *file_path(clip_feed_handle_t h, const char *id, const char *suffix)
{
    size_t size=strlen(h->root)+strlen(id)+strlen(suffix)+2; char *path=malloc(size);
    if (path) snprintf(path,size,"%s/%s%s",h->root,id,suffix);
    return path;
}
static bool text_valid(const cJSON *object, const char *key, size_t max)
{
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsString(value)&&strlen(value->valuestring)<=max;
}
static bool valid(const char *id, const cJSON *payload)
{
    const cJSON *kind=cJSON_GetObjectItemCaseSensitive(payload,"kind");
    const cJSON *version=cJSON_GetObjectItemCaseSensitive(payload,"schemaVersion");
    const cJSON *time=cJSON_GetObjectItemCaseSensitive(payload,"fetchedAt");
    const cJSON *stale=cJSON_GetObjectItemCaseSensitive(payload,"staleAfterMs");
    const cJSON *cards=cJSON_GetObjectItemCaseSensitive(payload,"cards");
    if (!cJSON_IsString(kind)||strcmp(kind->valuestring,id)||!cJSON_IsNumber(version)||version->valuedouble!=1||
        !cJSON_IsNumber(time)||!isfinite(time->valuedouble)||time->valuedouble<1||time->valuedouble>9007199254740991.0||
        !cJSON_IsNumber(stale)||!isfinite(stale->valuedouble)||stale->valuedouble<60000||stale->valuedouble>86400000||
        !cJSON_IsArray(cards)||cJSON_GetArraySize(cards)<1||cJSON_GetArraySize(cards)>FEED_CARDS_MAX) return false;
    const cJSON *card;
    cJSON_ArrayForEach(card,cards) if (!text_valid(card,"title",384)||!text_valid(card,"value",1536)||!text_valid(card,"detail",512)) return false;
    return true;
}
static char *read_file(const char *path)
{
    struct stat st; if (stat(path,&st)||st.st_size<=0||st.st_size>FEED_BYTES_MAX) return NULL;
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    char *text=calloc((size_t)st.st_size+1,1);
    if (text&&fread(text,1,(size_t)st.st_size,f)!=(size_t)st.st_size) { free(text); text=NULL; }
    fclose(f); return text;
}
esp_err_t clip_feed_create(const char *data_root, clip_feed_handle_t *out)
{
    if (!data_root||!out) return ESP_ERR_INVALID_ARG;
    *out=NULL; clip_feed_handle_t h=calloc(1,sizeof(*h)); if (!h) return ESP_ERR_NO_MEM;
    size_t size=strlen(data_root)+sizeof("/clip_feeds"); h->root=malloc(size);
    if (!h->root) { clip_feed_delete(h); return ESP_ERR_NO_MEM; }
    snprintf(h->root,size,"%s/clip_feeds",data_root);
    if (mkdir(h->root,0755)&&errno!=EEXIST) { clip_feed_delete(h); return ESP_FAIL; }
    for (int i=0;i<3;i++) {
        char *live=file_path(h,ids[i],".json"),*old=file_path(h,ids[i],".old"),*stage=file_path(h,ids[i],".new");
        if (!live||!old||!stage) { free(live);free(old);free(stage);clip_feed_delete(h);return ESP_ERR_NO_MEM; }
        if (access(live,F_OK)&&!access(old,F_OK)) rename(old,live);
        unlink(stage);
        char *text=read_file(live);cJSON *payload=text?cJSON_Parse(text):NULL;
        if (payload&&valid(ids[i],payload)) { h->slots[i].text=text;unlink(old); } else free(text);
        cJSON_Delete(payload);free(live);free(old);free(stage);
    }
    *out=h;return ESP_OK;
}
void clip_feed_delete(clip_feed_handle_t h) { if (h) { for (int i=0;i<3;i++)free(h->slots[i].text);free(h->root);free(h); } }
esp_err_t clip_feed_update(clip_feed_handle_t h, const char *id, const cJSON *payload, uint64_t now_ms)
{
    int index=index_of(id); if (!h||index<0||!valid(id,payload)) return ESP_ERR_INVALID_ARG;
    char *text=cJSON_PrintUnformatted(payload); if (!text) return ESP_ERR_NO_MEM;
    size_t size=strlen(text);if (size>FEED_BYTES_MAX) { cJSON_free(text);return ESP_ERR_INVALID_SIZE; }
    char *live=file_path(h,id,".json"),*old=file_path(h,id,".old"),*stage=file_path(h,id,".new");
    esp_err_t err=ESP_ERR_NO_MEM;
    if (!live||!old||!stage) goto done;
    err=ESP_FAIL;FILE *f=fopen(stage,"wb");if (!f)goto done;
    bool written=fwrite(text,1,size,f)==size;
    if (fflush(f)) written=false;
    if (fsync(fileno(f))&&errno!=ENOSYS&&errno!=EINVAL) written=false;
    if (fclose(f)) written=false;
    if (!written) { unlink(stage);goto done; }
    unlink(old);bool had_live=!access(live,F_OK);
    if (had_live&&rename(live,old)) { unlink(stage);goto done; }
    if (rename(stage,live)) { if (had_live)rename(old,live);unlink(stage);goto done; }
    const cJSON *age=cJSON_GetObjectItemCaseSensitive(payload,"ageAtSendMs");
    uint64_t initial_age=cJSON_IsNumber(age)&&isfinite(age->valuedouble)&&age->valuedouble>0?(uint64_t)fmin(age->valuedouble,604800000):0;
    unlink(old);free(h->slots[index].text);h->slots[index]=(feed_slot_t){.text=text,.received_ms=now_ms,.initial_age_ms=initial_age,.fresh=true};
    text=NULL;err=ESP_OK;
done:
    cJSON_free(text);free(live);free(old);free(stage);return err;
}
esp_err_t clip_feed_read(clip_feed_handle_t h, const char *id, uint64_t now_ms, char **payload, int64_t *age_ms)
{
    int index=index_of(id);if (!h||index<0||!payload||!age_ms)return ESP_ERR_INVALID_ARG;
    *payload=NULL;feed_slot_t *slot=&h->slots[index];if (!slot->text)return ESP_ERR_NOT_FOUND;
    *payload=strdup(slot->text);if (!*payload)return ESP_ERR_NO_MEM;
    *age_ms=slot->fresh?(int64_t)(slot->initial_age_ms+(now_ms>=slot->received_ms?now_ms-slot->received_ms:0)):-1;return ESP_OK;
}

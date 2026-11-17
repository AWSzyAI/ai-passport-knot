#include "knot_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef struct { char key[16]; unsigned char bytes[96]; size_t size; int blob; } item_t;
static item_t durable[1024], staged[1024];
static int fail_commit;
struct iterator { int index; };
static int locate(const char *key, int create) {
    int empty=-1;
    for(int i=0;i<1024;i++) {
        if(!strcmp(staged[i].key,key)) return i;
        if(!staged[i].key[0] && empty<0) empty=i;
    }
    if(create && empty>=0) { snprintf(staged[empty].key,16,"%s",key); return empty; }
    return -1;
}
esp_err_t nvs_flash_init_partition(const char*p) { assert(!strcmp(p,"knots")); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char*p,const char*n,int mode,nvs_handle_t*h) {
    (void)p;(void)n;(void)mode; memcpy(staged,durable,sizeof(staged)); *h=1; return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h,const char*k,uint32_t*v) {
    (void)h; int i=locate(k,0); if(i<0)return ESP_ERR_NVS_NOT_FOUND;
    assert(staged[i].size==sizeof(*v));memcpy(v,staged[i].bytes,sizeof(*v));return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h,const char*k,uint32_t v) {
    (void)h; int i=locate(k,1);assert(i>=0);memcpy(staged[i].bytes,&v,sizeof(v));staged[i].size=sizeof(v);return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*v,size_t*s) {
    (void)h;int i=locate(k,0);if(i<0)return ESP_ERR_NVS_NOT_FOUND;
    if(*s<staged[i].size)return ESP_ERR_INVALID_SIZE;
    *s=staged[i].size;memcpy(v,staged[i].bytes,*s);return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*v,size_t s) {
    (void)h;int i=locate(k,1);assert(i>=0 && s<=96);
    memcpy(staged[i].bytes,v,s);staged[i].size=s;staged[i].blob=1;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;if(fail_commit>0 && --fail_commit==0)return ESP_FAIL;
    memcpy(durable,staged,sizeof(durable));return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char*k) {
    (void)h;int i=locate(k,0);if(i<0)return ESP_ERR_NVS_NOT_FOUND;
    memset(&staged[i],0,sizeof(staged[i]));return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it) {
    for(int i=(*it)->index+1;i<1024;i++) if(staged[i].key[0] && staged[i].blob) { (*it)->index=i;return ESP_OK; }
    free(*it);*it=NULL;return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_find(const char*p,const char*n,int t,nvs_iterator_t*it) {
    (void)p;(void)n;(void)t;*it=malloc(sizeof(**it));assert(*it);(*it)->index=-1;return nvs_entry_next(it);
}
void nvs_entry_info(nvs_iterator_t it,nvs_entry_info_t*info) { memcpy(info->key,staged[it->index].key,16); }
void nvs_release_iterator(nvs_iterator_t it) { free(it); }

int main(void) {
    uint32_t boot; knot_event_t first,second,read;
    assert(knot_store_init(&boot)==ESP_OK && boot==1);
    assert(knot_store_add(1000000,&first)==ESP_OK && !first.utc_ms);
    assert(knot_store_sync(11000000,1794844800000LL)==ESP_OK);
    assert(knot_store_first(&read)==ESP_OK && read.utc_ms==1794844790000LL);
    /* Lost ACK and reboot retain the frozen timestamp and the same ID. */
    assert(knot_store_init(&boot)==ESP_OK && boot==2);
    assert(knot_store_first(&second)==ESP_OK && second.seq==first.seq && second.utc_ms==read.utc_ms);
    knot_history_t h[KNOT_HISTORY];
    assert(knot_store_history(h,KNOT_HISTORY)==1 && h[0].seq==first.seq && !KNOT_HISTORY_SYNCED(&h[0]) && h[0].utc_ms==read.utc_ms);
    assert(knot_store_ack(first.seq)==ESP_OK && knot_store_count()==0);
    assert(knot_store_history(h,KNOT_HISTORY)==1 && KNOT_HISTORY_SYNCED(&h[0])); /* green after ACK */
    assert(knot_store_add(2000000,&second)==ESP_OK && !second.utc_ms);
    assert(knot_store_init(&boot)==ESP_OK && boot==3);
    assert(knot_store_sync(5000000,1794844900000LL)==ESP_OK);
    assert(knot_store_first(&read)==ESP_OK && !read.utc_ms); /* never map old boot */
    assert(knot_store_ack(read.seq)==ESP_OK);
    /* Failure after durable ID reservation: no record, no ID reuse after reboot. */
    fail_commit=2;
    assert(knot_store_add(6000000,&read)==ESP_FAIL);
    uint32_t skipped=read.seq;
    assert(knot_store_init(&boot)==ESP_OK);
    assert(knot_store_count()==0);
    assert(knot_store_add(1,&read)==ESP_OK && read.seq>skipped);
    /* Failure before an ACK commits: reboot must replay the record. */
    fail_commit=1;
    assert(knot_store_ack(read.seq)==ESP_FAIL);
    assert(knot_store_init(&boot)==ESP_OK && knot_store_count()==1);
    assert(knot_store_first(&read)==ESP_OK);
    assert(knot_store_ack(read.seq)==ESP_OK);
    /* ACK committed but history write lost before power cut: reboot marks it synced. */
    assert(knot_store_add(7,&read)==ESP_OK);
    fail_commit=2; /* 1st commit = erase (ok), 2nd = history update (lost) */
    assert(knot_store_ack(read.seq)==ESP_OK);
    assert(knot_store_init(&boot)==ESP_OK);
    assert(knot_store_history(h,1)==1 && h[0].seq==read.seq && KNOT_HISTORY_SYNCED(&h[0]));
    /* Kinds, voice finishing, scan, and v1 upgrade of an old stored blob. */
    assert(knot_store_add_kind(8,KNOT_KIND_START,KNOT_NO_SLOT,&read)==ESP_OK && read.flags==KNOT_FLAG_AUDIO_DONE);
    assert(knot_store_history(h,1)==1 && KNOT_HISTORY_KIND(&h[0])==KNOT_KIND_START);
    assert(knot_store_ack(read.seq)==ESP_OK);
    assert(knot_store_add_kind(9,KNOT_KIND_VOICE,4,&read)==ESP_OK && read.audio_slot==4 && !read.flags);
    assert(knot_store_finish_audio(read.seq,1234,0xabcd)==ESP_OK);
    assert(knot_store_init(&boot)==ESP_OK);
    knot_event_t voice; assert(knot_store_first(&voice)==ESP_OK);
    assert(voice.kind==KNOT_KIND_VOICE && voice.audio_bytes==1234 && voice.audio_crc==0xabcd && (voice.flags&KNOT_FLAG_AUDIO_DONE));
    assert(knot_store_ack(voice.seq)==ESP_OK);
    { knot_event_v1_t old={.version=1,.seq=voice.seq+1,.boot=1,.mono_us=3,.utc_ms=1794844800000LL};
      old.crc=knot_crc(&old,offsetof(knot_event_v1_t,crc));
      char k[16]; snprintf(k,16,"e%08x",voice.seq+1); /* simulate a record written by firmware v1 */
      nvs_set_u32(1,"next",voice.seq+2); nvs_set_blob(1,k,&old,sizeof(old)); nvs_commit(1);
      assert(knot_store_init(&boot)==ESP_OK && knot_store_count()==1);
      assert(knot_store_first(&read)==ESP_OK && read.version==2 && read.seq==old.seq && read.kind==KNOT_KIND_TIME);
      assert(knot_store_ack(read.seq)==ESP_OK); }
    for(unsigned i=0;i<512;i++) assert(knot_store_add(i,&read)==ESP_OK);
    assert(knot_store_add(999,&read)==ESP_ERR_NO_MEM);
    assert(knot_store_count()==512);
    /* History survives reboot, newest first, capped, pending shown as unsynced. */
    assert(knot_store_init(&boot)==ESP_OK);
    unsigned n=knot_store_history(h,KNOT_HISTORY);
    assert(n==KNOT_HISTORY && h[0].seq==read.seq && !KNOT_HISTORY_SYNCED(&h[0]));
    for(unsigned i=1;i<n;i++) assert(h[i].seq==h[i-1].seq-1);
    assert(knot_store_history(h,5)==5);
    puts("knot_store: PASS (reboot, clock anchors, lost ACK, failed commits, full queue, history)");
}

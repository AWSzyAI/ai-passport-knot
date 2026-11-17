#include "knot_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char key[16]; unsigned char bytes[80]; size_t size; int blob; } item_t;
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
    (void)h;int i=locate(k,1);assert(i>=0 && s<=80);
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
    assert(knot_store_history(h,KNOT_HISTORY)==1 && h[0].seq==first.seq && !h[0].synced && h[0].utc_ms==read.utc_ms);
    assert(knot_store_ack(first.seq)==ESP_OK && knot_store_count()==0);
    assert(knot_store_history(h,KNOT_HISTORY)==1 && h[0].synced); /* green after ACK */
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
    assert(knot_store_history(h,1)==1 && h[0].seq==read.seq && h[0].synced);
    for(unsigned i=0;i<512;i++) assert(knot_store_add(i,&read)==ESP_OK);
    assert(knot_store_add(999,&read)==ESP_ERR_NO_MEM);
    assert(knot_store_count()==512);
    /* History survives reboot, newest first, capped, pending shown as unsynced. */
    assert(knot_store_init(&boot)==ESP_OK);
    unsigned n=knot_store_history(h,KNOT_HISTORY);
    assert(n==KNOT_HISTORY && h[0].seq==read.seq && !h[0].synced);
    for(unsigned i=1;i<n;i++) assert(h[i].seq==h[i-1].seq-1);
    assert(knot_store_history(h,5)==5);
    puts("knot_store: PASS (reboot, clock anchors, lost ACK, failed commits, full queue, history)");
}

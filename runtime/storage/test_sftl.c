#include "sftl.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint32_t random_word=0xa35f7621;
static uint32_t random32(void) {
    random_word^=random_word<<13;random_word^=random_word>>17;
    random_word^=random_word<<5;return random_word;
}
int main(void) {
    struct ns_sftl m;struct ns_run runs[512];struct ns_update updates[128];
    uint8_t mask[4]={15,7,3,0};uint32_t node[1024],v;
    for(unsigned round=0;round<10000;++round) {
        for(unsigned j=0;j<1024;++j)node[j]=random32();
        assert(ns_sftl_init(&m,4,100,mask,runs,512,updates,128,1,5)==0);
        assert(ns_sftl_node(&m,node)==NS_FORMAT);
        assert(ns_sftl_finish(&m)==NS_FORMAT);
        assert(ns_sftl_lookup(&m,0,&v)==NS_NOT_READY);
    }
    memset(node,255,sizeof node);node[0]=0;node[1]=0xfffffff0;
    node[2]=0;node[3]=100;
    assert(!ns_sftl_init(&m,4,100,mask,runs,512,updates,128,1,5));
    assert(!ns_sftl_node(&m,node));
    uint32_t record[4]={6u<<16|1,1,17,1};
    assert(!ns_sftl_replay(&m,2048,record));
    assert(!ns_sftl_finish(&m));
    assert(!ns_sftl_lookup(&m,17,&v)&&v==2048);
    assert(ns_sftl_ready_block(&m,1,1)==NS_CONFLICT);
    assert(!ns_sftl_ready_block(&m,2,1));
    assert(ns_sftl_ready_block(&m,2,9)==NS_ARGUMENT);
    /* One corrupt later node invalidates the mount, even after earlier success. */
    node[1023]=0;
    assert(ns_sftl_node(&m,node)==NS_FORMAT);
    assert(ns_sftl_finish(&m)==NS_FORMAT);
    assert(ns_sftl_lookup(&m,17,&v)==NS_NOT_READY);
    puts("PASS SFTL sanitizers: malformed nodes, sticky mount failure, replay, live-block erase guard");
}

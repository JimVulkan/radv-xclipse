#!/usr/bin/env python3
"""Regressions for four Xclipse failure-handling fixes.

Extracts production function bodies with controlled allocation, Vulkan and DRM
test doubles. This does not build the full driver or execute on an Android GPU.
Use --ref COMMIT --expect-bugs to reproduce the pre-fix behavior as a control.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('repo', type=Path, nargs='?', default=Path(__file__).resolve().parents[1])
ap.add_argument('--ref', help='Read production functions from this Git ref')
ap.add_argument('--expect-bugs', action='store_true', help='Reproduce the pre-fix version as a control')
ap.add_argument('--output', type=Path, default=Path('audit_results.json'))
args = ap.parse_args()
ROOT = args.repo.resolve()
FIXED = not args.expect_bugs
OUT = args.output.resolve()
LOG = OUT.parent / 'logs'
LOG.mkdir(parents=True, exist_ok=True)

def source(path):
    if args.ref:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', args.ref + ':' + path], text=True)
    return (ROOT / path).read_text()

def function(path, name):
    s = source(path)
    m = re.search(r'(?m)^(?:[A-Za-z_][A-Za-z_0-9 *]*\s+)?' + re.escape(name) + r'\(', s)
    assert m, name
    start = s.rfind('\n', 0, m.start() - 1) + 1
    b = s.index('{', m.end())
    depth, end = 1, b + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'

results = []
def run(name, unit, expected_error=None):
    with tempfile.TemporaryDirectory(prefix='radv-review-') as d:
        c, exe = Path(d) / 'test.c', Path(d) / 'test'
        c.write_text(unit)
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-pthread',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src'),
                        str(c), '-o', str(exe)], check=True, capture_output=True, text=True)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0')
        p = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=5)
        log = p.stdout + p.stderr
        (LOG / (name + '.log')).write_text(log)
        if expected_error:
            assert p.returncode != 0 and re.search(expected_error, log), (name, log)
            status = 'REPRODUCED'
        else:
            assert p.returncode == 0, (name, log)
            status = 'REPRODUCED' if p.stdout.startswith('REPRO:') else 'PASS'
        entry = {'test': name, 'status': status, 'exit_code': p.returncode,
                 'evidence': next((l for l in log.splitlines() if 'runtime error:' in l or 'ERROR: AddressSanitizer' in l), p.stdout.strip()),
                 'log': 'logs/' + name + '.log'}
        results.append(entry)
        print(name + ': ' + status, flush=True)

COMMON = '#define EXPECT_FIXED ' + str(int(FIXED)) + '\n' + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define MAX2(a,b) ((a)>(b)?(a):(b))
'''
CS = 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c'
CS_TYPES = r'''
#define align(n,a) (((n)+(a)-1)&~((a)-1))
#define C_3F3_IB_SIZE 0xfff00000u
#define G_3F3_IB_SIZE(n) ((n)&0xfffffu)
#define PKT3(a,b,c) 0x1234u
#define PKT3_INDIRECT_BUFFER 1
#define S_3F3_CHAIN(n) ((n)<<1)
#define S_3F3_VALID(n) (n)
struct ac_cmdbuf { uint32_t *buf; unsigned cdw, max_dw, reserved_dw; };
struct radeon_winsys_bo { uint64_t va, size; };
struct radv_amdgpu_ib { struct radeon_winsys_bo *bo; uint64_t va; unsigned cdw; };
struct radv_amdgpu_cs;
struct radeon_winsys {
   VkResult (*cs_finalize)(struct ac_cmdbuf *);
   void (*cs_add_buffer)(struct ac_cmdbuf *, struct radeon_winsys_bo *);
   void (*buffer_destroy)(struct radeon_winsys *, struct radeon_winsys_bo *);
};
struct radv_amdgpu_winsys { struct radeon_winsys base; struct { struct { unsigned ib_alignment; } ip[1]; } info; };
struct radv_amdgpu_cs {
   struct ac_cmdbuf base; struct radv_amdgpu_winsys *ws;
   struct radeon_winsys_bo *ib_buffer; uint8_t *ib_mapped;
   struct radv_amdgpu_ib *ib_buffers; unsigned num_ib_buffers, max_num_ib_buffers;
   unsigned *ib_size_ptr; VkResult status; void *chained_to;
   bool chain_ib; unsigned hw_ip; uint32_t *last_cp_dma_header;
};
static struct radv_amdgpu_cs *radv_amdgpu_cs(struct ac_cmdbuf *b) { return (void *)b; }
struct radv_amdgpu_winsys_bo { struct radeon_winsys_bo base; };
static struct radv_amdgpu_winsys_bo *radv_amdgpu_winsys_bo(struct radeon_winsys_bo *b) { return (void *)b; }
static struct radeon_winsys_bo old_bo={.va=4096}, new_bo={.va=8192};
static uint32_t old_map[32], new_map[32];
static unsigned mode, allocation_calls;
static void noop_add(struct ac_cmdbuf *c, struct radeon_winsys_bo *b) {}
static void noop_destroy(struct radeon_winsys *w, struct radeon_winsys_bo *b) {}
static VkResult finalize_for_grow(struct ac_cmdbuf *b) {
   struct radv_amdgpu_cs *c=(void *)b;
   if(mode==3){c->status=VK_ERROR_OUT_OF_HOST_MEMORY;return c->status;}
   c->ib_buffers[c->num_ib_buffers++].bo=c->ib_buffer;
   c->ib_buffer=NULL; b->cdw=8; return VK_SUCCESS;
}
static VkResult radv_amdgpu_cs_bo_create(struct radv_amdgpu_cs *c, uint32_t n) {
   allocation_calls++;
   c->ib_buffer=mode==1?NULL:&new_bo;
   return mode==1?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
static void *radv_buffer_map(struct radeon_winsys *w, struct radeon_winsys_bo *b) {
   if(mode==2 && b==&new_bo) return NULL;
   return b==&old_bo?old_map:new_map;
}
'''
grow = COMMON + CS_TYPES + function(CS, 'radv_amdgpu_restore_last_ib') + function(CS, 'radv_amdgpu_cs_grow')
setup = r'''
struct radv_amdgpu_winsys ws={.base={finalize_for_grow,noop_add,noop_destroy},.info.ip[0].ib_alignment=16};
struct radv_amdgpu_ib old_ibs[2]={0};
struct radv_amdgpu_cs c={.base={old_map,4,28,28},.ws=&ws,.ib_buffer=&old_bo,
   .ib_mapped=(void *)old_map,.ib_buffers=old_ibs,.max_num_ib_buffers=2,.status=VK_SUCCESS};
'''
run('cs_grow_allocation_failure', grow + 'int main(void){' + setup + r'''
mode=1; c.chain_ib=true; radv_amdgpu_cs_grow(&c.base,4);
assert(c.status==VK_ERROR_OUT_OF_DEVICE_MEMORY && c.ib_buffer==&old_bo && c.base.buf==old_map);
assert(c.ib_mapped==(void *)old_map && c.base.max_dw==28 && c.num_ib_buffers==0);
puts("PASS: failed allocation preserves ownership, mapping and capacity without patching a chain"); return 0; }
''', None if FIXED else r'AddressSanitizer|runtime error:')
run('cs_grow_mapping_failure', grow + 'int main(void){' + setup + r'''
mode=2; radv_amdgpu_cs_grow(&c.base,4);
assert(c.status==VK_ERROR_OUT_OF_DEVICE_MEMORY && c.ib_buffer==&old_bo);
if (EXPECT_FIXED) {
 assert(c.base.buf==old_map && c.ib_mapped==(void *)old_map && c.base.max_dw==28 && c.num_ib_buffers==0);
 puts("PASS: failed map preserves the previous recording pointer and BO");
} else {
 assert(c.base.buf==NULL);puts("REPRO: old BO restored, but recording buffer pointer overwritten with NULL");
} }
''')
run('cs_grow_success_control', grow + 'int main(void){' + setup + r'''
mode=0; c.chain_ib=true; radv_amdgpu_cs_grow(&c.base,4);
assert(c.status==VK_SUCCESS && c.ib_buffer==&new_bo && c.base.buf==new_map);
puts("PASS: successful grow preserves a valid recording buffer"); }
''')
run('cs_grow_finalize_failure', grow + 'int main(void){' + setup + r'''
mode=3; radv_amdgpu_cs_grow(&c.base,4);
assert(c.status==VK_ERROR_OUT_OF_HOST_MEMORY);
if(EXPECT_FIXED){
 assert(allocation_calls==0 && c.ib_buffer==&old_bo && c.base.buf==old_map && c.num_ib_buffers==0);
 puts("PASS: failed finalization stops grow before allocation or ownership changes");
}else{
 assert(allocation_calls==1 && c.ib_buffer==&new_bo);
 puts("REPRO: grow ignores failed finalization and allocates another IB");
} }
''')
finalize_prelude = r'''
static void *failed_realloc(void *old, size_t size) { return NULL; }
#define realloc failed_realloc
static void radv_amdgpu_winsys_cs_pad(struct ac_cmdbuf *c, unsigned n) {}
static void radv_amdgpu_cs_emit_nops(struct radv_amdgpu_cs *c, unsigned n) { c->base.cdw+=n; }
'''
run('cs_finalize_owner_loss', COMMON + CS_TYPES + finalize_prelude + function(CS, 'radv_amdgpu_cs_add_ib_buffer') + function(CS, 'radv_amdgpu_cs_finalize') + r'''
int main(void){
struct radv_amdgpu_cs c={.base={old_map,4,28,28},.ib_buffer=&old_bo,.status=VK_SUCCESS};
VkResult r=radv_amdgpu_cs_finalize(&c.base);
assert(r==VK_ERROR_OUT_OF_HOST_MEMORY && c.num_ib_buffers==0);
if (EXPECT_FIXED) {
 assert(c.ib_buffer==&old_bo);unsigned cdw=c.base.cdw;
 assert(radv_amdgpu_cs_finalize(&c.base)==r && c.base.cdw==cdw);
 puts("PASS: failed realloc retains ownership; repeated finalize does not pad or transfer it");
} else { assert(c.ib_buffer==NULL);puts("REPRO: realloc failure loses the only owned BO reference"); } }
''')

GR = 'src/util/u_gralloc/u_gralloc_fallback.c'
gr_unit = COMMON + r'''
typedef struct { int version,numFds,numInts; int data[4]; } native_handle_t;
''' + function(GR, 'get_native_buffer_fds') + ('\n#define GET_FDS(h,f) get_native_buffer_fds(h,f,3)\n' if FIXED else '\n#define GET_FDS(h,f) get_native_buffer_fds(h,f)\n')
run('gralloc_four_fds', gr_unit + r'''
int main(void){ native_handle_t h={.numFds=4,.data={10,11,12,13}};
int fds[3]={7,8,9};
if (EXPECT_FIXED) { assert(GET_FDS(&h,fds)==0 && fds[0]==7 && fds[2]==9);
 puts("PASS: oversized descriptor list rejected without writing output");
} else { GET_FDS(&h,fds); } return 0; }
''', None if FIXED else r'stack-buffer-overflow|runtime error:')
run('gralloc_three_fds_control', gr_unit + r'''
int main(void){ native_handle_t h={.numFds=3,.data={10,11,12}};
int fds[3]; assert(GET_FDS(&h,fds)==3 && fds[2]==12);
puts("PASS: three descriptors fit"); }
''')

SYNC = 'src/util/u_sync_provider.c'
sync = COMMON + r'''
#define HAVE_LIBDRM 1
#include "util/u_sync_provider.h"
struct util_sync_provider_drm { struct util_sync_provider base; int drm_fd; };
static void *fail_calloc(void){ return NULL; }
#define CALLOC_STRUCT(T) ((struct T *)fail_calloc())
#define DRM_CAP_SYNCOBJ_TIMELINE 1
static int drmGetCap(int fd,uint64_t cap,uint64_t *v) { *v=0; return 0; }
'''
for name in ['create','destroy','handle_to_fd','fd_to_handle','import_sync_file','export_sync_file',
             'wait','reset','signal','query','transfer','timeline_signal','timeline_wait']:
    # Use the exact callback signatures from the production header.
    hdr=source('src/util/u_sync_provider.h')
    m=re.search(r'int \(\*'+name+r'\)\((.*?)\);', hdr, re.S)
    sync+='static int drm_syncobj_'+name+'('+m.group(1)+'){ return 0; }\n'
sync += r'''
static void drm_syncobj_finalize(struct util_sync_provider *p) {}
static struct util_sync_provider *drm_sync_provider_clone(struct util_sync_provider *p) { return NULL; }
'''
sync += function(SYNC,'util_sync_provider_drm')
run('sync_provider_allocation_failure', sync + r'''
int main(void){ assert(util_sync_provider_drm(3)==NULL);puts("PASS: provider allocation failure returns NULL");return 0; }
''', None if FIXED else r'member access within null pointer|AddressSanitizer')

# Lightweight Vulkan definitions and the production ringbuffer macros.
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "vulkan/vulkan_core.h"
#include "vulkan/vk_android_native_buffer.h"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define ASSERTED
#define UNUSED __attribute__((unused))
#define ONE_MS_IN_NS INT64_C(1000000)
#define ONE_SECOND_IN_NS INT64_C(1000000000)
#define p_atomic_read(p) (*(p))
#define p_atomic_set(p,v) (*(p) = (v))
#define VK_FROM_HANDLE(type, name, handle) struct type *name = (void *)(handle)
typedef unsigned simple_mtx_t;
enum { mtx_plain };
static void simple_mtx_lock(simple_mtx_t *m) { assert(!*m); *m = 1; }
static void simple_mtx_unlock(simple_mtx_t *m) { assert(*m); *m = 0; }
/* Use Mesa's real ringbuffer macros, with single-threaded test locks. */
#define UTIL_MACROS_H
#define _SIMPLE_MTX_H
static void simple_mtx_init(simple_mtx_t *m, int flags) { (void)flags; *m = 0; }
#include "util/ringbuffer.h"
struct hash_entry { void *key, *data; };
struct hash_table { struct hash_entry entry; unsigned entries; };
#define hash_table_foreach(table, e) \
   for (struct hash_entry *e = (table)->entries ? &(table)->entry : NULL; e; e = NULL)
static struct hash_entry *_mesa_hash_table_search(struct hash_table *t, const void *key)
{ return t->entries && t->entry.key == key ? &t->entry : NULL; }
struct vk_frame_time_info { uint64_t frame_time; int64_t min_delay; };
typedef void (*vk_frame_time_cb)(const struct vk_frame_time_info *, void *);
struct vk_frame_pacer;
struct vk_device {
   struct vk_frame_pacer *frame_pacer;
   struct {
      PFN_vkWaitSemaphores WaitSemaphores;
      PFN_vkGetQueryPoolResults GetQueryPoolResults;
      PFN_vkResetQueryPool ResetQueryPool;
   } dispatch_table;
};
struct vk_queue { struct { struct vk_device *device; } base; };
static struct vk_queue *vk_queue_from_handle(VkQueue q) { return (void *)q; }
static VkDevice vk_device_to_handle(struct vk_device *d) { return (void *)d; }
'''
runtime=source('src/vulkan/runtime/vk_frame_pacer.c')
preamble=PRELUDE.replace('struct vk_frame_pacer *frame_pacer;\n   struct {',
   'struct vk_frame_pacer *frame_pacer;\n   VkAllocationCallbacks alloc; VkTimeDomainKHR calibrate_time_domain;\n   struct {')
preamble=preamble.replace('struct vk_queue { struct { struct vk_device *device; } base; };',
   'struct vk_queue { struct { struct vk_device *device; } base; VkDeviceQueueCreateFlags flags; };')
pacer='#define EXPECT_FIXED ' + str(int(FIXED)) + '\n' + preamble+runtime[runtime.index('#define MAX_FRAMES'):runtime.index('\nvoid\nvk_frame_pacer_calibrate')]
pacer+=r'''
#include <stdlib.h>
static bool fail_alloc, requested_unreachable_infinite_wait;
static VkResult driver_error;
static unsigned driver_calls;
static uint64_t completed;
static VkResult vk_device_get_timestamp(struct vk_device *d,VkTimeDomainKHR domain,uint64_t *v) { *v=1000;return VK_SUCCESS; }
struct multi { void **outputs[8]; size_t sizes[8]; unsigned n; };
#define VK_MULTIALLOC(m) struct multi m={0}
static void multi_add(struct multi *m,void **out,size_t n){m->outputs[m->n]=out;m->sizes[m->n++]=n;}
#define vk_multialloc_add(m,p,T,n) multi_add(m,(void **)(p),sizeof(T)*(n))
static void *vk_multialloc_zalloc(struct multi *m,const VkAllocationCallbacks *a,VkSystemAllocationScope s){
   if(fail_alloc)return NULL;
   size_t n=0;for(unsigned i=0;i<m->n;i++)n+=(m->sizes[i]+15)&~(size_t)15;
   char *b=calloc(1,n),*p=b;
   for(unsigned i=0;i<m->n;i++){*m->outputs[i]=p;p+=(m->sizes[i]+15)&~(size_t)15;}return b;
}
static void vk_free(const VkAllocationCallbacks *a,void *p){ free(p); }
static VkResult VKAPI_CALL driver_submit(VkQueue q,uint32_t n,const VkSubmitInfo2 *s,VkFence f){
   driver_calls++;if(driver_error)return driver_error;for(unsigned i=0;i<n;i++)for(unsigned j=0;j<s[i].signalSemaphoreInfoCount;j++)
      completed=s[i].pSignalSemaphoreInfos[j].value;return VK_SUCCESS;
}
static VkResult VKAPI_CALL wait_semaphores(VkDevice d,const VkSemaphoreWaitInfo *i,uint64_t timeout){
   if(*i->pValues>completed){if(timeout==UINT64_MAX)requested_unreachable_infinite_wait=true;return VK_TIMEOUT;}
   return VK_SUCCESS;
}
static VkResult VKAPI_CALL query_results(VkDevice d,VkQueryPool p,uint32_t first,uint32_t n,size_t size,void *data,VkDeviceSize stride,VkQueryResultFlags flags){
   for(unsigned i=0;i<n;i++)*(uint64_t *)((char *)data+i*stride)=1000+i*100;return VK_SUCCESS;
}
static void VKAPI_CALL reset_queries(VkDevice d,VkQueryPool p,uint32_t first,uint32_t n){}
'''
for name in ['evaluate_frame','allocate_query','get_timestamp_queries','frame_pacer_submit',
             'frame_pacer_QueueSubmit2','vk_frame_pacer_disable']:
    pacer+=function('src/vulkan/runtime/vk_frame_pacer.c',name)
pacer_main=r'''
struct vk_frame_pacer p={.enabled=true,.queue_submit2=driver_submit,.calibration.timestamp_period=1};
struct vk_device d={.frame_pacer=&p,.dispatch_table={wait_semaphores,query_results,reset_queries}};
struct vk_queue q={.base.device=&d};p.device=&d;
queue_context ctx={.latency_sensitive=true};
p.queues=(struct hash_table){.entry={&q,&ctx},.entries=1};
ringbuffer_init(p.frames);ringbuffer_init(ctx.queries);p.active_frame=ringbuffer_alloc(p.frames);
VkCommandBufferSubmitInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
VkSemaphoreSubmitInfo dummy_signal={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
VkSubmitInfo2 s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.commandBufferInfoCount=1,.pCommandBufferInfos=&cmd,.pSignalSemaphoreInfos=&dummy_signal};
'''
run('pacer_failed_submit_retains_unsignalled_point', pacer+'int main(void){'+pacer_main+r'''
fail_alloc=true;
assert(frame_pacer_QueueSubmit2((VkQueue)&q,1,&s,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY);
assert(driver_calls==0 && completed==0);
if (EXPECT_FIXED) {
 assert(ctx.signal_value[0]==0 && ctx.num_submits[0]==0 && ctx.queries.size==0 && ctx.queries.head==0 && ctx.semaphore_value==0);
 vk_frame_pacer_disable(&p);assert(!requested_unreachable_infinite_wait);
 puts("PASS: pre-submit OOM rolls back query and timeline reservations; disable has no unsent work to wait for");
} else {
 assert(ctx.signal_value[0]==1 && ctx.num_submits[0]==1 && ctx.queries.size==2);
 vk_frame_pacer_disable(&p);assert(requested_unreachable_infinite_wait);
 puts("REPRO: allocation failure leaves an unreachable timeline value");
} }
''')
run('pacer_submit_success_control', pacer+'int main(void){'+pacer_main+r'''
fail_alloc=false;
assert(frame_pacer_QueueSubmit2((VkQueue)&q,1,&s,VK_NULL_HANDLE)==VK_SUCCESS);
vk_frame_pacer_disable(&p);
assert(driver_calls==1 && completed==1 && !requested_unreachable_infinite_wait && ctx.queries.size==0);
puts("PASS: successful submission signals the point and disable drains queries"); }
''')

for error in ['VK_ERROR_OUT_OF_HOST_MEMORY', 'VK_ERROR_OUT_OF_DEVICE_MEMORY']:
    run('pacer_driver_' + error.lower(), pacer + 'int main(void){' + pacer_main + r'''
assert(frame_pacer_QueueSubmit2((VkQueue)&q,1,&s,VK_NULL_HANDLE)==VK_SUCCESS);
assert(completed==1 && ctx.queries.size==2);
driver_error=INJECTED_ERROR;
assert(frame_pacer_QueueSubmit2((VkQueue)&q,1,&s,VK_NULL_HANDLE)==driver_error);
assert(driver_calls==2 && completed==1);
if(EXPECT_FIXED){
 assert(ctx.signal_value[0]==1 && ctx.num_submits[0]==1 && ctx.queries.size==2 && ctx.queries.head==2 && ctx.semaphore_value==1);
 vk_frame_pacer_disable(&p);assert(!requested_unreachable_infinite_wait && ctx.queries.size==0);
 puts("PASS: driver OOM rolls back only the failed submit and preserves earlier successful work");
}else{
 assert(ctx.signal_value[0]==2 && ctx.num_submits[0]==2);
 vk_frame_pacer_disable(&p);assert(requested_unreachable_infinite_wait);
 puts("REPRO: driver OOM leaves a timeline point with no signal");
} }
'''.replace('INJECTED_ERROR', error))

# Real pthread locks and scheduling hooks exercise disable racing with a failed
# driver submission. Both Vulkan queue synchronization modes must roll back first.
threaded='#include <pthread.h>\n'+pacer
threaded=threaded.replace('typedef unsigned simple_mtx_t;',
    'typedef pthread_mutex_t simple_mtx_t;\nstatic void test_lock_hook(simple_mtx_t *m);')
threaded=threaded.replace('static void simple_mtx_lock(simple_mtx_t *m) { assert(!*m); *m = 1; }',
    'static void simple_mtx_lock(simple_mtx_t *m) { test_lock_hook(m);assert(!pthread_mutex_lock(m)); }')
threaded=threaded.replace('static void simple_mtx_unlock(simple_mtx_t *m) { assert(*m); *m = 0; }',
    'static void simple_mtx_unlock(simple_mtx_t *m) { assert(!pthread_mutex_unlock(m)); }')
threaded=threaded.replace('static void simple_mtx_init(simple_mtx_t *m, int flags) { (void)flags; *m = 0; }',
    'static void simple_mtx_init(simple_mtx_t *m, int flags) { (void)flags;assert(!pthread_mutex_init(m,NULL)); }')
threaded=threaded.replace('static VkResult VKAPI_CALL driver_submit(',
    'static void driver_gate(void);\nstatic VkResult VKAPI_CALL driver_submit(')
threaded=threaded.replace('driver_calls++;if(driver_error)', 'driver_calls++;driver_gate();if(driver_error)')
threaded=threaded.replace('static VkResult VKAPI_CALL wait_semaphores(',
    'static void test_wait_hook(void);\nstatic VkResult VKAPI_CALL wait_semaphores(')
threaded=threaded.replace('   if(*i->pValues>completed)', '   test_wait_hook();if(*i->pValues>completed)')
threaded+=r'''
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static bool driver_entered,disable_queries_attempted;
static simple_mtx_t *target_queries;
static _Thread_local unsigned role;
static VkQueue submit_queue;
static VkSubmitInfo2 *submit_info;
static struct vk_frame_pacer *disable_pacer;
static VkResult submission_result;
static void test_wait_hook(void){
 if(role==2){pthread_mutex_lock(&gate);disable_queries_attempted=true;
  pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);}
}
static void test_lock_hook(simple_mtx_t *m){
 if(role==2 && m==target_queries){
  pthread_mutex_lock(&gate);disable_queries_attempted=true;
  pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
 }
}
static void driver_gate(void){
 pthread_mutex_lock(&gate);driver_entered=true;pthread_cond_broadcast(&cond);
 while(!disable_queries_attempted)pthread_cond_wait(&cond,&gate);
 pthread_mutex_unlock(&gate);
}
static void *submit_thread(void *unused){role=1;
 submission_result=frame_pacer_QueueSubmit2(submit_queue,1,submit_info,VK_NULL_HANDLE);return NULL;
}
static void *disable_thread(void *unused){role=2;vk_frame_pacer_disable(disable_pacer);return NULL;}
'''
for flags in ['0','VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR']:
    run('pacer_concurrent_disable_' + ('internal' if flags!='0' else 'external'),
        threaded+'int main(void){'+pacer_main+r'''
q.flags=QUEUE_FLAGS;target_queries=&ctx.queries.mtx;
submit_queue=(VkQueue)&q;submit_info=&s;disable_pacer=&p;
driver_error=VK_ERROR_OUT_OF_HOST_MEMORY;
pthread_t a,b;assert(!pthread_create(&a,NULL,submit_thread,NULL));
pthread_mutex_lock(&gate);while(!driver_entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
assert(!pthread_create(&b,NULL,disable_thread,NULL));pthread_join(a,NULL);pthread_join(b,NULL);
assert(submission_result==VK_ERROR_OUT_OF_HOST_MEMORY);
if(EXPECT_FIXED){assert(!requested_unreachable_infinite_wait && ctx.queries.size==0 && ctx.num_submits[0]==0);
 puts("PASS: concurrent disable observes the failed-submit rollback before waiting");
}else{assert(requested_unreachable_infinite_wait);
 puts("REPRO: concurrent disable waits for a failed driver submission");}
return 0;}
'''.replace('QUEUE_FLAGS',flags))

device_unit = COMMON + r'''
#include <errno.h>
typedef void *amdgpu_device_handle;
struct util_sync_provider { void (*finalize)(struct util_sync_provider *); };
typedef struct ac_drm_device {
   amdgpu_device_handle adev; struct util_sync_provider *p; int fd; bool is_virtio;
} ac_drm_device;
static bool provider_fails;
static unsigned drm_refs, finalized;
static int amdgpu_device_initialize(int fd,uint32_t *ma,uint32_t *mi,amdgpu_device_handle *a){
   drm_refs++;*a=(void *)(uintptr_t)1;return 0;
}
static int amdgpu_device_get_fd(amdgpu_device_handle a){return 7;}
static void amdgpu_device_deinitialize(amdgpu_device_handle a){assert(drm_refs);drm_refs--;}
static void finalize_provider(struct util_sync_provider *p){finalized++;}
static struct util_sync_provider provider={finalize_provider};
static struct util_sync_provider *util_sync_provider_drm(int fd){return provider_fails?NULL:&provider;}
'''
device_unit += function('src/amd/common/ac_linux_drm.c','ac_drm_device_initialize')
device_unit += function('src/amd/common/ac_linux_drm.c','ac_drm_device_deinitialize')
run('drm_device_provider_failure_cleanup', device_unit + r'''
int main(void){uint32_t ma=0,mi=0;ac_drm_device *dev=NULL;provider_fails=true;
int r=ac_drm_device_initialize(7,false,&ma,&mi,&dev);
if(EXPECT_FIXED){assert(r==-ENOMEM && dev==NULL && drm_refs==0 && finalized==0);
 puts("PASS: provider failure releases the DRM reference and clears the output");
}else{assert(r==0 && dev && !dev->p && drm_refs==1);
 puts("REPRO: initialization succeeds with a NULL synchronization provider");
 amdgpu_device_deinitialize(dev->adev);free(dev);
} }
''')
run('drm_device_provider_success_control', device_unit + r'''
int main(void){uint32_t ma=0,mi=0;ac_drm_device *dev=NULL;
assert(ac_drm_device_initialize(7,false,&ma,&mi,&dev)==0 && dev && dev->p && drm_refs==1);
ac_drm_device_deinitialize(dev);assert(!drm_refs && finalized==1);
puts("PASS: successful provider initialization and cleanup retain balanced ownership");}
''')

probe_path='src/vulkan/runtime/vk_drm_syncobj.c'
probe_body=function(probe_path,'vk_drm_syncobj_get_type_from_provider')
callback_fields=dict(re.findall(r'\.(\w+) = (vk_\w+)',probe_body))
callback_fields.update(dict(re.findall(r'type\.(\w+) = (vk_\w+)',probe_body)))
probe=COMMON+r'''
struct util_sync_provider {
 int (*create)(struct util_sync_provider *,uint32_t,uint32_t *);
 int (*wait)(struct util_sync_provider *,uint32_t *,unsigned,int64_t,unsigned,uint32_t *);
 int (*destroy)(struct util_sync_provider *,uint32_t);
 void *timeline_wait;
 void (*finalize)(struct util_sync_provider *);
};
struct vk_drm_syncobj { int unused; };
#define DRM_SYNCOBJ_CREATE_SIGNALED 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
static struct util_sync_provider *util_sync_provider_drm(int fd){return NULL;}
'''
probe+='struct vk_sync_type { size_t size; unsigned features;'+''.join('void (*'+f+')(void);' for f in callback_fields)+'};\n'
probe+='\n'.join('#define '+c+' 1' for c in set(re.findall(r'VK_SYNC_FEATURE_\w+',probe_body)))+'\n'
probe+='\n'.join('static void '+n+'(void){}' for n in set(callback_fields.values()))+'\n'
probe+=probe_body+function(probe_path,'vk_drm_syncobj_get_type')
run('sync_type_probe_without_provider',probe+r'''
int main(void){assert(!vk_drm_syncobj_get_type_from_provider(NULL).features);
assert(!vk_drm_syncobj_get_type(7).features);
puts("PASS: synchronization type probes handle unavailable providers without dereferencing NULL");}
''',None if FIXED else r'member access within null pointer|AddressSanitizer')

OUT.write_text(json.dumps({'base_commit': subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
  'scope':'Extracted production C functions, injected failures and mocked Vulkan/DRM; no full driver build, GPU or Android execution',
  'tests':results},indent=2)+'\n')
print(str(OUT))

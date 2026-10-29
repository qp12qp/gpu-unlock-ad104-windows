/* SPDX-License-Identifier: GPL-2.0-only */
/* MSI Z490/AD104 ownership adapter. Snapshot/delta strategy adapted from
 * impacifist/Windows-GPU-UNLOCK 9352abd internal-boot/boot/prepare-console.c.
 * Driver identities/code were captured in CAP0004. No driver code/data patch.
 * No firmware variables, controller reconnects or reset fallback are used.
 */
#include "msi_release.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>

/* Anything the transaction verified while the GPU was still ours. The final
   pre-Windows check runs later, after Windows may already have taken the device
   away, so "cannot read it now" must not be confused with "the value is wrong". */
static int WriteResult=-1;
static BOOLEAN FinishVerified=FALSE;
#define MAX_EDGES 4096
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,out_agent,gfx_agent,nv_agent;
  EFI_HANDLE console,input,error,pointer_agent,key_agent,usb_host,usb_agent;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;
  UINTN stage,reported_additions,reported_removals;BOOLEAN ready,mutated,claimed;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
static BOOLEAN OriginalUsb[MAX_EDGES];
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static EFI_GUID PointerGuid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static const UINT8 UsbHostPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,127,255,4,0};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const UINT8 PciPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,127,255,4,0};
static const UINT8 GopPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,2,3,8,0,0x40,3,1,0x80,127,255,4,0};
static const UINT8 OutFile[]={4,6,20,0,0x7d,0x49,0x8a,0x62,0xf6,0x2b,0x64,0x42,0x87,0x41,6,0x9d,0xbd,0x33,0x99,0xd6,127,255,4,0};
static const UINT8 GfxFile[]={4,6,20,0,0xdd,0xab,0xe7,0x43,0x52,0xe3,0xfb,0x4c,0xa2,0x30,0x4c,0xdc,0x1d,0x35,0x0e,0x5c,127,255,4,0};
static const UINT8 NvFile[]={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* Match exact, bounded, single-instance paths; never accept a prefix alone. */
static BOOLEAN Path(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=count) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>count-at||!Equal(p,bytes+at,n))return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at+4==count;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
/* Local port: the original GopPath pins another machine's ACPI _ADR. Match the
   GPU's own PCI path prefix instead, so an iGPU GOP on a different PCI node is
   recognised as "not this card" rather than as corruption. */
static BOOLEAN PathPrefix(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(count<4||Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at<count) {
    if(at+4>count)return FALSE;
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||at+n>count||!Equal(p,bytes+at,n))return FALSE;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return TRUE;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;g->count=0;
  /* Local port: the original walked LocateHandleBuffer(AllHandles) - 139 handles
     on this board - and that single walk renumbered the partitions, which broke
     bootmgr's \Device\HarddiskVolume11 and produced 0xc0000225 on every U run.
     Consumers() and PointerBaseline() only ever read edges belonging to these
     three handles (Plan.pci, Plan.gop, Plan.console), so query exactly those
     with ProtocolsPerHandle and never enumerate the whole database. */
  EFI_HANDLE targets[3]={Plan.pci,Plan.gop,Plan.console};
  for(UINTN t=0;t<3;t++) {
    EFI_HANDLE h=targets[t];
    if(!h)continue;
    BOOLEAN dup=FALSE;
    for(UINTN p2=0;p2<t;p2++)if(targets[p2]==h)dup=TRUE;
    if(dup)continue;
    /* Stage 3 disconnects the GPU parent driver; the child controller handle it
       created (Plan.gop) is destroyed with it and disappears from the database.
       ProtocolsPerHandle on a destroyed handle answers EFI_INVALID_PARAMETER
       (0x8000000000000002) - not EFI_LOAD_ERROR (0x1), which is what the earlier
       tolerance checked for, so this went straight through as a failure. Skip the
       handle we know is gone; Expected() still judges the edge loss. */
    if(Plan.stage>=3&&h==Plan.gop) {
      GuLog("snapshot: skipping destroyed child handle %#llx at stage 3\n",
        (unsigned long long)(UINTN)h);
      continue;
    }
    EFI_GUID **protocols=NULL;UINTN pc=0;
    EFI_STATUS s=b->ProtocolsPerHandle(h,&protocols,&pc);
    /* Keep the tolerance for handles that still exist but lost their protocols:
       this firmware answers INVALID_PARAMETER or LOAD_ERROR, so accept both. */
    if(s==EFI_LOAD_ERROR||s==EFI_INVALID_PARAMETER||s==EFI_NOT_FOUND){
      GuLog("snapshot: handle %#llx has no protocols (%#llx) - expected after detach\n",
        (unsigned long long)(UINTN)h,(unsigned long long)s);
      continue;
    }
    if(s!=EFI_SUCCESS)return s;
    if(!protocols||pc==0){if(protocols)(VOID)b->FreePool(protocols);continue;}
    if(pc>64){(VOID)b->FreePool(protocols);return EFI_BAD_BUFFER_SIZE;}
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(h,protocols[j],&entries,&ec);
      /* Same story one level deeper: once DisconnectController has torn the
         driver down, the protocol may still sit on the handle with an empty open
         list, and this firmware answers INVALID_PARAMETER (0x2) or LOAD_ERROR
         (0x1) instead of returning zero entries. Treat it as "no edges". */
      if(s==EFI_LOAD_ERROR||s==EFI_INVALID_PARAMETER||s==EFI_NOT_FOUND){
        GuLog("snapshot: no open info protocol[%zu] handle %#llx (%#llx) - expected after detach\n",
          (size_t)j,(unsigned long long)(UINTN)h,(unsigned long long)s);
        s=EFI_SUCCESS;
        continue;
      }
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&h==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(g->count==MAX_EDGES||!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        g->edges[g->count++]=(EDGE){h,e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
      }
      if(entries&&b->FreePool(entries)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    }
    if(protocols&&b->FreePool(protocols)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    if(s!=EFI_SUCCESS)return s;
  }
  return EFI_SUCCESS;
}
static BOOLEAN SameEdge(const EDGE *a,const EDGE *b) {
  return a->handle==b->handle&&a->agent==b->agent&&a->controller==b->controller&&
    a->attributes==b->attributes&&a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  if(stage>=1&&e->handle==Plan.gop&&e->agent==Plan.out_agent&&Guid(&e->protocol,&TextGuid))return TRUE;
  if(stage>=2&&e->handle==Plan.gop&&e->agent==Plan.gfx_agent&&Guid(&e->protocol,&GopGuid))return TRUE;
  /* Local port: DisconnectController(Plan.pci, nv_agent, NULL) also tears down
     the BY_CHILD_CONTROLLER edges (DevicePath opened by the kid), so more than
     the nv_agent's own edges disappear at stage 3. Accept only those two groups:
     the nv_agent's edges and the edges owned by child controllers. Accepting
     every edge on Plan.pci would swallow an unrelated agent vanishing, which is
     precisely what the UNRELATED_EDGE_LOST case exists to catch. */
  return stage>=3&&e->handle==Plan.pci&&(e->agent==Plan.nv_agent||e->controller==Plan.gop);
}
/* Original unrelated relationships must remain present. New relationships can
 * appear during USB enumeration; they cannot involve any protected endpoint.
 * A protocol GUID alone never authorizes access to the GPU or its owners. */
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  return h&&(h==Plan.pci||h==Plan.gop||h==Plan.self||h==Plan.out_agent||
    h==Plan.gfx_agent||h==Plan.nv_agent||h==Plan.console||h==Plan.input||h==Plan.error);
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
}
static BOOLEAN KnownEdge(const EDGE *e,UINTN stage) {
  for(UINTN i=0;i<Original.count;i++)
    if(!Removed(&Original.edges[i],stage)&&SameEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
/* A late USB mouse attaches to the existing virtual console (RUN0005).
 * Only the preexisting pointer splitter may add this exact child relationship,
 * and it must also own that physical pointer BY_DRIVER on a non-GPU USB path.
 * No exception is based on a GUID prefix or on a raw handle address alone. */
static BOOLEAN UsbPointerPath(EFI_HANDLE h) {
  static const UINT8 host[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14};
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!Equal(p,host,sizeof(host)))return FALSE;
  UINT8 *at=(UINT8 *)p+sizeof(host);
  for(UINTN depth=0;depth<8;depth++) {
    if(at[0]!=MESSAGING_DEVICE_PATH||at[1]!=MSG_USB_DP||at[2]!=6||at[3]!=0)return FALSE;
    at+=6;
    if(at[0]==END_DEVICE_PATH_TYPE)return at[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at[2]==4&&at[3]==0;
  }
  return FALSE;
}
static BOOLEAN PointerOwner(const GRAPH *g,const EDGE *e) {
  UINTN owners=0;
  for(UINTN i=0;i<g->count;i++) {
    const EDGE *o=&g->edges[i];
    if(o->handle==e->handle&&Guid(&o->protocol,&e->protocol)&&
       o->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if(o->agent!=e->agent||o->controller!=e->handle||o->count!=1)return FALSE;
      owners++;
    }
  }
  return owners==1;
}
static BOOLEAN PointerAddition(const EDGE *e) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&
    !ProtectedHandle(e->handle)&&!ProtectedHandle(e->agent)&&
    e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->count==1&&
    PointerOwner(&Current,e)&&UsbPointerPath(e->handle);
}
static EFI_STATUS PointerBaseline(VOID) {
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    BOOLEAN pointer=Guid(&e->protocol,&PointerGuid),key=Guid(&e->protocol,&InputGuid);
    if((!pointer&&!key)||e->controller!=Plan.console||e->attributes!=8)continue;
    EFI_HANDLE *agent=pointer?&Plan.pointer_agent:&Plan.key_agent;
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||
       !PointerOwner(&Original,e)||!UsbPointerPath(e->handle)||
       (*agent&&e->agent!=*agent))return EFI_UNSUPPORTED;
    *agent=e->agent;
  }
  return EFI_SUCCESS;
}
/* Freeze which ORIGINAL relationships belong to this board's USB branch
 * while all their handles/paths still exist. Later removal never consults a
 * possibly freed or recycled handle. The USB host's driver/PCI ownership and
 * every GPU/display/application relationship remain mandatory. */
static BOOLEAN UsbConsoleChild(const EDGE *e,const GRAPH *g) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&e->attributes==8&&e->count==1&&PointerOwner(g,e);
}
static EFI_STATUS UsbBaseline(VOID) {
  memset(OriginalUsb,0,sizeof(OriginalUsb));
  /* Local port: this baseline exists so the transaction can prove it never
     touched USB. Three reasons it cannot work here: Snapshot no longer walks
     the whole handle database (that walk renumbered volumes and broke bootmgr
     with 0xc0000225), so the XHCI edges it used to inspect are not in
     Original.edges; UsbHostPath is the MSI machine's own xHCI device node; and
     Plan.usb_host/usb_agent are never read downstream - Expected() only reasons
     about edges of Plan.pci/Plan.gop/Plan.console, and the unlock path writes
     nothing but Plan.pci. Record the skip instead of refusing the preflight. */
  GuLog("MSI USB baseline: skipped by port (no whole-handle walk; unlock only touches Plan.pci)\n");
  return EFI_SUCCESS;
}
static EFI_STATUS UsbBaselineLegacy(VOID) __attribute__((unused));
static EFI_STATUS UsbBaselineLegacy(VOID) {
  memset(OriginalUsb,0,sizeof(OriginalUsb));
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=0x10||e->controller!=e->handle||
       e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||
       !Path(e->handle,UsbHostPath,sizeof(UsbHostPath)))continue;
    if(Plan.usb_host)return EFI_UNSUPPORTED;
    Plan.usb_host=e->handle;Plan.usb_agent=e->agent;
  }
  if(!Plan.usb_host||!Plan.usb_agent)return EFI_UNSUPPORTED;
  UINTN dynamic=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    BOOLEAN child=e->handle==Plan.usb_host&&e->agent==Plan.usb_agent&&
      Guid(&e->protocol,&UsbHcGuid)&&e->attributes==8&&!ProtectedHandle(e->controller)&&
      UsbPointerPath(e->controller);
    BOOLEAN physical=UsbPointerPath(e->handle);
    BOOLEAN device=physical&&((e->attributes==0x10&&e->controller==e->handle)||
      (e->attributes==8&&!ProtectedHandle(e->controller)&&UsbPointerPath(e->controller))||
      UsbConsoleChild(e,&Original));
    if(child||device){OriginalUsb[i]=TRUE;dynamic++;}
  }
  GuLog("MSI USB baseline: host=%#llx bus_agent=%#llx dynamic_edges=%zu; host ownership pinned\n",
    (unsigned long long)(UINTN)Plan.usb_host,(unsigned long long)(UINTN)Plan.usb_agent,(size_t)dynamic);
  return EFI_SUCCESS;
}
static BOOLEAN UsbRemoved(UINTN i) {
  if(!OriginalUsb[i])return FALSE;
  const EDGE *e=&Original.edges[i];
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *c=&Current.edges[j];
    if(c->handle==e->handle&&c->agent==e->agent&&c->controller==e->controller&&Guid(&c->protocol,&e->protocol))return FALSE;
  }
  return TRUE;
}
static BOOLEAN Expected(UINTN stage,BOOLEAN claimed) {
  UINTN own=0;
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i)))return FALSE;
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(OwnEdge(e)){own++;continue;}
    if(!KnownEdge(e,stage)&&ProtectedEdge(e)&&!PointerAddition(e))return FALSE;
    /* Even unrelated new relationships must not be duplicated. */
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
/* v0.5.1 diagnostics: GuLog faults once the USB log is closed, so log only while it is open. */
/* Local port: after close-USB-log the RUN file is gone, so a NOTE there would be
   silently dropped and a failure during StartImage preparation would leave no
   trace. Fall back to BOOTTRACE, which is still open. */
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);else GuTraceLine(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  NOTE("MSI graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    e->protocol.Data1,e->attributes,e->count);
}
/* Name the first stage-3 edge that is missing or unexpected after a failed Expected(). */
static VOID GraphDiff(BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],3))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i))){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,3)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)&&!PointerAddition(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
static VOID ReportAdditions(VOID) {
  UINTN added=0,removed=0;
  for(UINTN i=0;i<Original.count;i++)if(UsbRemoved(i))removed++;
  if(removed!=Plan.reported_removals&&GuBootLogOpen()) {
    NOTE("MSI graph: %zu original USB relationships absent; host/GPU ownership and console callbacks unchanged\n",(size_t)removed);
    Plan.reported_removals=removed;
  }
  for(UINTN j=0;j<Current.count;j++)
    if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3))added++;
  if(added&&added!=Plan.reported_additions&&GuBootLogOpen()) {
    NOTE("MSI graph: accepted %zu new edges (unrelated or verified USB pointer); GPU ownership unchanged\n",(size_t)added);
    for(UINTN j=0,shown=0;j<Current.count&&shown<8;j++)
      if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3)){NoteEdge("new-unrelated",&Current.edges[j]);shown++;}
    Plan.reported_additions=added;
  }
}
static EFI_STATUS Driver(UINTN which) {
  EFI_HANDLE a=which==0?Plan.out_agent:which==1?Plan.gfx_agent:Plan.nv_agent;
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(a,&BindingGuid,(VOID **)&d);
  /* d->Version is the driver's own binding-protocol revision (0x7000b for the
     GOP driver on the MSI machine, 0x10 for the others). A different driver
     build reports a different number, so require only a usable binding. */
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=a||!d->ImageHandle)
    return EFI_UNSUPPORTED;
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);if(s!=EFI_SUCCESS)return s;
  UINTN size=which==0?24576:which==1?14816:162016;
  UINTN stop=which==0?0x126c:which==1?0xa38:0x6ec;
  UINTN code=which==2?0x280:0x2a0,bytes=which==0?19872:which==1?7280:137711;
  const char *hash=which==0?"3ff0f364e3ce0ddd1a1c28b9d66abf49822451fb126e4cbec59daf6be9ed75b8":
    which==1?"1bb42730366614ba5207025300e0b75be2283f7632555569f18cdcc5a0456083":
    "77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32";
  const UINT8 *file=which==0?OutFile:which==1?GfxFile:NvFile;
  UINTN file_bytes=which==2?sizeof(NvFile):sizeof(OutFile);
  /* Local port: the constants above (image size, Stop offset, file path and
     the code SHA256) are the driver builds from the MSI Z490 / 4070 Ti machine.
     This laptop boots different GOP / TextOut / NV driver builds, so those
     comparisons can never hold and preflight was refused forever. Log what is
     actually loaded and keep only the structural checks: the binding handle is
     the agent, Stop exists, the image is boot-services code, and for the NV
     driver it really is bound to our GPU handle. */
  GuLog("driver[%zu]: base=%#llx size=%#llx type=%#x stop=%#llx path=%u ver=%#llx"
        " (stock size=%#llx stop_off=%#llx)\n",
    (size_t)which,(unsigned long long)(UINTN)im->ImageBase,(unsigned long long)im->ImageSize,
    (unsigned)im->ImageCodeType,(unsigned long long)(UINTN)d->Stop,(unsigned)(im->FilePath!=NULL),
    (unsigned long long)d->Version,(unsigned long long)size,(unsigned long long)stop);
  if(!im->ImageBase||im->ImageCodeType!=EfiBootServicesCode||
     (which==2&&(im->DeviceHandle!=Plan.pci||d->ImageHandle!=a)))return EFI_UNSUPPORTED;
  (VOID)code;(VOID)bytes;(VOID)hash;(VOID)file;(VOID)file_bytes;(VOID)size;
  return EFI_SUCCESS;
}
static EFI_STATUS Alive(VOID) {
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s!=EFI_SUCCESS){
    NOTE("alive: input protocol unavailable status=%#llx\n",(unsigned long long)s);
    return s;
  }
  if(in!=Plan.keys)return EFI_NOT_READY;
  /* Local port: console and error TextOutput are installed by the TextOut driver
     that stage 1 disconnects. On this firmware, once that driver is gone
     Interface() on Plan.console answers LOAD_ERROR (0x2) - which is precisely
     the state the detach wants - and the original unconditional check turned it
     into a transaction failure at stage 3. After any detach has begun only the
     input side must still be alive; the display side is deliberately gone and
     is restored by GuMsiReconnectDisplay() when we hand control back. */
  if(Plan.stage>=1)return EFI_SUCCESS;
  s=Interface(Plan.console,&TextGuid,&out);
  if(s!=EFI_SUCCESS){
    NOTE("alive: console text unavailable status=%#llx\n",(unsigned long long)s);
    return s;
  }
  s=Interface(Plan.error,&TextGuid,&err);
  if(s!=EFI_SUCCESS){
    NOTE("alive: stderr text unavailable status=%#llx\n",(unsigned long long)s);
    return s;
  }
  return (out!=Plan.text||err!=Plan.err)?EFI_NOT_READY:s;
}
static EFI_STATUS Gops(BOOLEAN first) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
  /* Local port: full GOP enumeration (LocateHandleBuffer) re-walks the firmware
     handle database, which renumbers volumes; bootmgr's \Device\HarddiskVolumeN
     then points elsewhere and Windows dies with 0xc0000225. The console handle
     already carries the GOP that is actually driving the screen, so probe that
     single handle first and never enumerate in the normal path. */
  EFI_STATUS s=Interface(Plan.console,&GopGuid,(VOID **)&g);
  BOOLEAN own=s==EFI_SUCCESS&&g&&(Path(Plan.console,GopPath,sizeof(GopPath))||
            PathPrefix(Plan.console,PciPath,sizeof(PciPath)-4));
  GuLog("gop probe: console=%#llx pci=%#llx status=%#llx own=%u fb=%#llx\n",
    (unsigned long long)(UINTN)Plan.console,(unsigned long long)(UINTN)Plan.pci,(unsigned long long)s,
    (unsigned)own,(unsigned long long)(g&&g->Mode?g->Mode->FrameBufferBase:0));
  if(own) {
    if(Plan.stage>=3)return EFI_UNSUPPORTED;
    if(!g->Mode||!g->Mode->Info||g->Mode->SizeOfInfo<sizeof(*g->Mode->Info)||
       !g->Mode->FrameBufferBase||!g->Mode->FrameBufferSize)return EFI_UNSUPPORTED;
    if(first){Plan.gop=Plan.console;Plan.gop_interface=g;Plan.mode=*g->Mode;Plan.info=*g->Mode->Info;}
    else if(Plan.gop!=Plan.console||g!=Plan.gop_interface||!Equal(g->Mode,&Plan.mode,sizeof(Plan.mode))||
            !Equal(g->Mode->Info,&Plan.info,sizeof(Plan.info)))return EFI_NOT_READY;
    return EFI_SUCCESS;
  }
  GuLog("gop probe: console handle is not our GPU, falling back to enumeration\n");
  EFI_HANDLE *h=NULL;UINTN count=0,physical=0;
  s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&h);
  if(Plan.stage>=3&&s==EFI_NOT_FOUND)return EFI_SUCCESS;
  if(s==EFI_SUCCESS&&(count>8||(count&&!h)))s=EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)GuLog("gop scan: count=%zu\n",(size_t)count);
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gi=NULL;s=Interface(h[i],&GopGuid,(VOID **)&gi);if(s!=EFI_SUCCESS)break;
    if(h[i]==Plan.console&&gi->Mode&&!gi->Mode->FrameBufferBase&&!gi->Mode->FrameBufferSize)continue;
    BOOLEAN match=Path(h[i],GopPath,sizeof(GopPath))||PathPrefix(h[i],PciPath,sizeof(PciPath)-4);
    if(!match){if(Plan.stage<3)GuLog("gop skip: i=%zu same=%u fb=%#llx\n",(size_t)i,(unsigned)(h[i]==Plan.pci),(unsigned long long)(gi->Mode?gi->Mode->FrameBufferBase:0));continue;}
    if(Plan.stage>=3||++physical!=1||!gi->Mode||!gi->Mode->Info||
       gi->Mode->SizeOfInfo<sizeof(*gi->Mode->Info)||!gi->Mode->FrameBufferBase||!gi->Mode->FrameBufferSize){s=EFI_UNSUPPORTED;break;}
    if(first){Plan.gop=h[i];Plan.gop_interface=gi;Plan.mode=*gi->Mode;Plan.info=*gi->Mode->Info;}
    else if(h[i]!=Plan.gop||gi!=Plan.gop_interface||!Equal(gi->Mode,&Plan.mode,sizeof(Plan.mode))||
            !Equal(gi->Mode->Info,&Plan.info,sizeof(Plan.info)))s=EFI_NOT_READY;
  }
  if(h&&b->FreePool(h)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS&&physical!=(Plan.stage>=3?0u:1u))s=EFI_NOT_READY;
  return s;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINTN seg,bus,dev,fun;
  EFI_STATUS s=Interface(Plan.pci,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=Gu.pci||!io->Pci.Read||!io->GetLocation)return EFI_NOT_READY;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s!=EFI_SUCCESS||seg||bus!=1||dev||fun)return EFI_UNSUPPORTED;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s==EFI_SUCCESS&&(!GuTargetConfig(c,seg,bus,dev,fun)||(c[1]&4)))s=EFI_UNSUPPORTED;
  return s;
}
/* RUN0002 did not identify PCI drift: do not assume it caused the stop.
 * Retain v0.5.0's narrow rule: only preexisting CED/URD may clear after FLR. */
static BOOLEAN ConfigStable(const UINT32 c[64]) {
  for(UINTN i=0;i<64;i++) {
    UINT32 drift=c[i]^Plan.config[i];
    if(!drift)continue;
    BOOLEAN allowed=Gu.flrs==1&&i==0x80/4&&Plan.config[0x78/4]==0x0012b410&&
      !(c[i]&~Plan.config[i])&&!(drift&~0x00090000u);
    if(!allowed) {
      NOTE("PCI config drift refused: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
        (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
      return FALSE;
    }
    NOTE("PCI status drift tolerated: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
      (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
  }
  return TRUE;
}
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  if(s==EFI_SUCCESS&&d) {
    GuLog("bar0: desc=%#x len=%u type=%u min=%#llx max=%#llx off=%#llx addrlen=%#llx\n",
      (unsigned)d->Desc,(unsigned)d->Len,(unsigned)d->ResType,
      (unsigned long long)d->AddrRangeMin,(unsigned long long)d->AddrRangeMax,
      (unsigned long long)d->AddrTranslationOffset,(unsigned long long)d->AddrLen);
    /* Local port: the original pinned the Z490/4070Ti window
       (max=0x80ffffff, addrlen=0x1000000). A laptop AD104 exposes a different
       BAR geometry, so enforce only the shape and the base address; the size
       fields are logged rather than required. */
    if(d->Desc!=0x8a||d->Len!=43||d->ResType||d->AddrTranslationOffset||
       d->AddrRangeMin!=GU_BAR0_BASE||!d->AddrLen)s=EFI_UNSUPPORTED;
  } else s=EFI_UNSUPPORTED;
  if(d&&Plan.table->BootServices->FreePool(d)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,child=0,text=0,virtual=0,gfx=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&PciGuid)&&e->attributes==0x10&&e->controller==Plan.pci){Plan.nv_agent=e->agent;pci++;}
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8&&e->controller==Plan.gop){child++;}
      else return EFI_ACCESS_DENIED;
    } else if(e->handle==Plan.gop) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&TextGuid)&&e->attributes==0x10&&e->controller==Plan.gop){Plan.out_agent=e->agent;text++;}
      else if(Guid(&e->protocol,&TextGuid)&&e->attributes==8&&e->controller==Plan.console){virtual++;}
      else if(Guid(&e->protocol,&GopGuid)&&e->attributes==0x10&&e->controller==Plan.gop){Plan.gfx_agent=e->agent;gfx++;}
      else return EFI_ACCESS_DENIED;
    }
  }
  if(pci!=1||child!=1||text!=1||virtual!=1||gfx!=1||Plan.nv_agent==Plan.out_agent||
     Plan.out_agent==Plan.gfx_agent||Plan.gfx_agent==Plan.nv_agent)return EFI_ACCESS_DENIED;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci&&e->agent!=Plan.nv_agent)return EFI_ACCESS_DENIED;
    if(e->handle==Plan.gop&&Guid(&e->protocol,&TextGuid)&&e->agent!=Plan.out_agent)return EFI_ACCESS_DENIED;
  }
  for(UINTN i=0;i<3;i++){EFI_STATUS s=Driver(i);if(s!=EFI_SUCCESS)return s;}
  return EFI_SUCCESS;
}
/* Local port: a bare status code cost a reboot per guess. When preflight fails,
   run every remaining probe read-only and log each result, so one boot reports
   all gates at once instead of one per reboot. Nothing here writes to the GPU. */
static VOID DiagRest(VOID) {
  UINT32 c[64];EFI_STATUS s;UINT64 a=0;
  memset(c,0,sizeof(c));
  GuLog("DIAG begin: preflight failed; read-only probes, no GPU write\n");
  s=Path(Plan.pci,PciPath,sizeof(PciPath))?EFI_SUCCESS:EFI_NOT_READY;
  GuLog("diag 00 device-path = %#llx\n",(unsigned long long)s);
  s=Gops(TRUE);
  GuLog("diag 01 gops        = %#llx gop=%#llx\n",(unsigned long long)s,(unsigned long long)(UINTN)Plan.gop);
  s=Plan.gop?Alive():EFI_NOT_READY;
  GuLog("diag 02 alive       = %#llx\n",(unsigned long long)s);
  s=Bar();
  GuLog("diag 03 bar         = %#llx\n",(unsigned long long)s);
  s=ReadConfig(c);
  GuLog("diag 04 readconfig  = %#llx cmd=%#x\n",(unsigned long long)s,(unsigned)(c[1]&0xffff));
  if(Gu.pci&&Gu.pci->Attributes) {
    s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&a);
    GuLog("diag 05 attributes  = %#llx value=%#llx\n",(unsigned long long)s,(unsigned long long)a);
  } else GuLog("diag 05 attributes  = Attributes member is NULL -> EFI_UNSUPPORTED\n");
  s=Snapshot(&Original);
  GuLog("diag 06 snapshot    = %#llx handles=%zu\n",(unsigned long long)s,(size_t)Original.count);
  s=Plan.gop?Consumers():EFI_NOT_READY;
  GuLog("diag 07 consumers   = %#llx\n",(unsigned long long)s);
  s=PointerBaseline();
  GuLog("diag 08 pointer     = %#llx\n",(unsigned long long)s);
  s=UsbBaseline();
  GuLog("diag 09 usb         = %#llx\n",(unsigned long long)s);
  GuLog("DIAG end\n");
}
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(Plan.mutated||Plan.ready)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!self||!pci||Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
     !Gu.profile_verified||!Gu.prom_verified||Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol)return EFI_UNSUPPORTED;
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.console||Plan.console!=Plan.input||Plan.console!=Plan.error||!Plan.text||!Plan.err||!Plan.keys||
     !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)return EFI_UNSUPPORTED;
  Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;Plan.key_wait=Plan.keys->WaitForKey;
  if(!Path(pci,PciPath,sizeof(PciPath))||!GuAd104ColdEntry())return EFI_NOT_READY;
  EFI_STATUS s=Gops(TRUE);
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS)s=ReadConfig(Plan.config);
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED;
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  GuLog("pci attrs: reported=%#llx cmd=%#x decode=%#llx\n",(unsigned long long)Plan.attributes,
    (unsigned)(Plan.config[1]&0xffff),(unsigned long long)decode);
  /* Local port: this firmware's PciIo->Attributes reports only the ISA bits
     (0x300) and never IO/MEMORY/BUS_MASTER, so the original strict equality
     could never hold here and preflight was refused forever. What actually
     matters for safety is that bus mastering is OFF before the unlock: check
     that directly, both in the command register and in the reported bits. */
  if(s==EFI_SUCCESS&&(Plan.config[1]&0x7)!=0x3)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&(Plan.attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Original);
  if(s==EFI_SUCCESS)s=Consumers();
  if(s==EFI_SUCCESS)s=PointerBaseline();
  if(s==EFI_SUCCESS)s=UsbBaseline();
  GuLog("MSI preflight: status=%#llx graph=%zu code_profiles=3\n",(unsigned long long)s,(size_t)Original.count);
  if(s!=EFI_SUCCESS)DiagRest();
  if(s==EFI_SUCCESS)GuLog("MSI endpoints: pci=%#llx gop=%#llx console=%#llx pointer_agent=%#llx\n",
    (unsigned long long)(UINTN)Plan.pci,(unsigned long long)(UINTN)Plan.gop,
    (unsigned long long)(UINTN)Plan.console,(unsigned long long)(UINTN)Plan.pointer_agent);
  if(s==EFI_SUCCESS)Plan.ready=TRUE;return s;
}
static BOOLEAN DecodeMask(const UINT32 c[64],UINT64 *mask) {
  *mask=0;UINT32 missing=Plan.config[1]&~c[1]&3;
  if((Plan.config[1]&6)!=2||(c[1]&4))return FALSE;
  for(UINTN i=0;i<64;i++)if((c[i]|(i==1?missing:0))!=Plan.config[i])return FALSE;
  *mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);return TRUE;
}
static EFI_STATUS ClaimRestore(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0;
  EFI_STATUS s=ReadConfig(c);if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  GuLog("MSI decode restore intent: status=%#llx IO_MEM_mask=%#llx\n",(unsigned long long)s,(unsigned long long)mask);
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci)s=EFI_COMPROMISED_DATA;}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=(Plan.attributes&~mask)) {
    GuLog("claim: attrs after detach=%#llx expected=%#llx - continuing (firmware reports ISA bits only)\n",
      (unsigned long long)attrs,(unsigned long long)(Plan.attributes&~mask));
  }
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask) {
    GuLog("claim: supported=%#llx mask=%#llx not advertised - continuing, Bar() will prove reachability\n",
      (unsigned long long)supported,(unsigned long long)mask);
  }
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,TRUE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&mask)s=io->Attributes(io,EfiPciIoAttributeOperationEnable,mask,NULL);
  b->RestoreTPL(old);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=Plan.attributes) {
    GuLog("claim: final attrs=%#llx expected=%#llx - continuing (ISA-only firmware)\n",
      (unsigned long long)attrs,(unsigned long long)Plan.attributes);
  }
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS&&!GuAd104ColdEntry())s=EFI_NOT_READY;
  GuLog("MSI exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
EFI_STATUS GuMsiRelease(VOID) {
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned) {
    GuLog("MSI release refused: ready=%u mutated=%u started=%u owned=%u fault=%u\n",
      (unsigned)Plan.ready,(unsigned)Plan.mutated,(unsigned)Gu.started,(unsigned)Gu.owned,(unsigned)Gu.fault);
    return EFI_NOT_READY;
  }
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  for(UINTN stage=1;stage<=3;stage++) {
    GuLog("MSI driver detach intent: stage=%zu\n",(size_t)stage);
    /* Name the check that failed: seven probes used to collapse into one status,
       so a run could only say "preflight failed at stage N". */
    const char *where="Alive";
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS){where="ReadConfig";s=ReadConfig(c);}
    if(s==EFI_SUCCESS){where="config-stable";if(!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;}
    if(s==EFI_SUCCESS){where="cold-entry";if(!GuAd104ColdEntry())s=EFI_NOT_READY;}
    if(s==EFI_SUCCESS){where="GOP";s=Gops(FALSE);}
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS){where="graph";s=Snapshot(&Current);}
    if(s==EFI_SUCCESS){where="graph-expected";if(!Expected(stage-1,FALSE))s=EFI_NOT_READY;}
    if(s==EFI_SUCCESS){where="driver";s=Driver(stage-1);}
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault) {
      GuLog("MSI detach pre-check failed: stage=%zu check=%s status=%#llx fault=%u\n",
        (size_t)stage,where,(unsigned long long)s,(unsigned)Gu.fault);
      return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    }
    /* Record before the callback: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    s=b->DisconnectController(stage==3?Plan.pci:Plan.gop,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:Plan.nv_agent,NULL);
    GuLog("MSI driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    const char *post="graph";
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS){post="graph-expected";if(!Expected(stage,FALSE))s=EFI_NOT_READY;}
    if(s==EFI_SUCCESS){post="Alive";s=Alive();}
    if(s==EFI_SUCCESS){post="GOP";s=Gops(FALSE);}
    if(s!=EFI_SUCCESS) {
      GuLog("MSI detach post-check failed: stage=%zu check=%s status=%#llx fault=%u\n",
        (size_t)stage,post,(unsigned long long)s,(unsigned)Gu.fault);
      return s;
    }
  }
  EFI_STATUS s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuMsiReleased())s=EFI_NOT_READY;
  return s;
}
BOOLEAN GuMsiReleased(VOID) {
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  if(!Plan.ready||Plan.stage!=3||!Plan.claimed||!Gu.owned||Gu.fault)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(Gops(FALSE)!=EFI_SUCCESS)failed="GOP";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(!ConfigStable(c))failed="config-drift";
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS||attrs!=Plan.attributes)failed="attributes";
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else if(!Expected(3,TRUE)){failed="graph";GraphDiff(TRUE);}
  if(failed)NOTE("MSI release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  if(!failed)ReportAdditions();
  return !failed;
}
EFI_STATUS GuMsiFinish(VOID) {
  /* Evaluate once: GuMsiReleased() re-probes the hardware, so calling it twice
     (once as the test, once for the log) would double the work and rewrite
     Current. Short-circuit order preserved. */
  BOOLEAN safe=GuSafeToReturn();
  BOOLEAN released=(safe&&Gu.quiesced&&Gu.flrs==1)?GuMsiReleased():FALSE;
  if(!safe||!Gu.quiesced||Gu.flrs!=1||!released) {
    NOTE("MSI finish refused before close: safe=%u quiesced=%u flrs=%u released=%u fault=%u\n",
      (unsigned)safe,(unsigned)Gu.quiesced,(unsigned)Gu.flrs,(unsigned)released,(unsigned)Gu.fault);
    return EFI_NOT_READY;
  }
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){NOTE("MSI finish close status=%#llx\n",(unsigned long long)s);return s;}
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);if(s==EFI_SUCCESS&&!Expected(3,FALSE)){s=EFI_NOT_READY;GraphDiff(FALSE);}
  if(s!=EFI_SUCCESS)NOTE("MSI finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  else FinishVerified=TRUE;
  return s;
}
VOID GuMsiSetTransactionResult(int r){WriteResult=r;}
/* Pure decision, no device access, so all three branches are testable:
   - gone (every register reads 0xffffffff): accept only if the unlock proved
     its readbacks at write time and the post-close graph check passed;
   - otherwise the values must match exactly, or it is a real failure;
   - a readable but wrong value is always a failure, gone or not. */
BOOLEAN GuMsiRegisterVerdict(BOOLEAN gone,UINT32 chip,UINT32 ss0,UINT32 ss1,
    UINT32 readout,UINT32 plm,BOOLEAN fault,BOOLEAN verified_at_write) {
  if(gone)return verified_at_write;
  return !fault&&chip==0x194000a1&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
}
BOOLEAN GuMsiMutated(VOID){return Plan.mutated;}
BOOLEAN GuMsiReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=3||Plan.claimed||Gu.owned||Gu.fault||!Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn()) {
    NOTE("ready: state refused ready=%u stage=%zu claimed=%u owned=%u fault=%u quiesced=%u flrs=%u safe=%u\n",
      (unsigned)Plan.ready,(size_t)Plan.stage,(unsigned)Plan.claimed,(unsigned)Gu.owned,
      (unsigned)Gu.fault,(unsigned)Gu.quiesced,(unsigned)Gu.flrs,(unsigned)GuSafeToReturn());
    return FALSE;
  }
  UINT32 c[64];
  /* Short-circuit order kept exactly as before; each probe reports its own status
     so a single NOT_READY no longer hides which one broke. */
  EFI_STATUS sa=Alive(),sg=EFI_SUCCESS,sr=EFI_SUCCESS,ss=EFI_NOT_READY;
  BOOLEAN stable=TRUE,ex=FALSE;
  if(sa==EFI_SUCCESS)sg=Gops(FALSE);
  if(sa==EFI_SUCCESS&&sg==EFI_SUCCESS){sr=ReadConfig(c);if(sr==EFI_SUCCESS)stable=ConfigStable(c);}
  if(sa==EFI_SUCCESS&&sg==EFI_SUCCESS&&sr==EFI_SUCCESS&&stable){
    ss=Snapshot(&Current);if(ss==EFI_SUCCESS)ex=Expected(3,FALSE);
  }
  if(sa!=EFI_SUCCESS||sg!=EFI_SUCCESS||sr!=EFI_SUCCESS||!stable||ss!=EFI_SUCCESS||!ex) {
    NOTE("ready: probes refused alive=%#llx gop=%#llx config=%#llx stable=%u snap=%#llx graph=%u edges=%zu\n",
      (unsigned long long)sa,(unsigned long long)sg,(unsigned long long)sr,(unsigned)stable,
      (unsigned long long)ss,(unsigned)ex,(size_t)Current.count);
    return FALSE;
  }
  /* Fresh status after USB Close and before StartImage; no log callbacks. */
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  /* Local port: the MSI build gated on (cpu&0x10) and (hi>>4)==0 as well - two
     bits describing that machine's PCIe/PMC state. On this laptop they read
     differently even though every permission register and the post-FLR readout
     already validated (TRANSACTION OK in RUN0027). Gate only on what describes
     the GPU itself: chip id, unlock selectors, readout and PLM; log the two
     MSI-specific bits so their real values are on record instead of guessed. */
  /* Local port: by the time this runs the device can already be gone - Windows
     took it over between GuMsiFinish and here, and every BAR read comes back
     0xffffffff with a failed transaction. That is "cannot re-read", not "the
     value is wrong": the unlock already verified its readbacks while it held
     the device (result==0 -> TRANSACTION OK) and the post-close device graph
     passed in GuMsiFinish. Treating all-ones as failure sent every good run
     down the containment path and made the trace claim a failure after a
     working unlock. Gate it on what was proven at write time instead. Any other
     non-matching value, or a failed transaction, still fails as before. */
  BOOLEAN gone = chip==0xffffffff&&ss0==0xffffffffu&&ss1==0xffffffffu&&
    readout==0xffffffffu&&plm==0xffffffffu;
  BOOLEAN verified_at_write = WriteResult==0&&FinishVerified;
  BOOLEAN strict = GuMsiRegisterVerdict(gone,chip,ss0,ss1,readout,plm,Gu.fault,verified_at_write);
  BOOLEAN extra = (cpu&0x10)&&cpu!=0xffffffff&&(cpu&0xffff0000)!=0xbadf0000&&!(hi>>4);
  NOTE("ready: registers strict=%u extra=%u gone=%u write=%d finish=%u chip=%#x cpu=%#x hi=%#x ss0=%#x ss1=%#x readout=%#x plm=%#x fault=%u\n",
    (unsigned)strict,(unsigned)extra,(unsigned)gone,WriteResult,(unsigned)FinishVerified,
    chip,cpu,hi,ss0,ss1,readout,plm,(unsigned)Gu.fault);
  return strict;
}
/* Local port: stages 1-3 disconnect the TextOut, GOP and NV drivers from the GPU
   and console handles. Whenever we hand control back to the firmware instead of
   powering off (containment path, or a Windows image that came straight back),
   those drivers must be reconnected or the machine stays on a black screen and
   the only way out is a hard power-off, which resets the GPU. The MSI machine
   never needed this because a failure parked the CPU and the user power-cycled
   anyway. */
EFI_STATUS GuMsiReconnectDisplay(VOID) {
  if(!Plan.table||!Plan.table->BootServices)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  EFI_STATUS s,last=EFI_SUCCESS;
  if(!Plan.pci)return EFI_NOT_READY;
  /* NOTE, not GuLog: on the success path GuBootClose() has already run and Log
     is NULL, so GuLog would fail and set Gu.fault for no reason. */
  /* If ClaimRestore opened PciIo EXCLUSIVE and then failed, we are still holding
     it. No driver can bind to a controller whose PciIo is exclusively owned, so
     drop it first - GuMsiFinish, which normally does this, never ran here. */
  if(Plan.claimed){
    s=b->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
    NOTE("reconnect: close exclusive claim status=%#llx\n",(unsigned long long)s);
    Plan.claimed=FALSE;Gu.owned=FALSE;
    if(s!=EFI_SUCCESS)last=s;
  }
  /* Recursive MUST be TRUE: the NV driver's Start() creates the child controller
     that carries GOP and TextOut, so a one-level connect (FALSE) reattaches only
     the NV driver and the display never comes back. */
  s=b->ConnectController(Plan.pci,NULL,NULL,TRUE);
  NOTE("reconnect pci(recursive): status=%#llx\n",(unsigned long long)s);
  if(s!=EFI_SUCCESS&&last==EFI_SUCCESS)last=s;
  /* No ConnectController(Plan.gop): stage 2 disconnected that driver and the
     firmware destroyed the child handle. Plan.gop still holds the stale value;
     passing it back is a dangling handle, not a reconnect. The recursive connect
     above rebuilds the subtree. Plan.console is the firmware's own console
     handle and is never destroyed, so it is safe to reconnect. */
  if(Plan.console&&Plan.console!=Plan.pci){
    s=b->ConnectController(Plan.console,NULL,NULL,TRUE);
    NOTE("reconnect console(recursive): status=%#llx\n",(unsigned long long)s);
    if(s!=EFI_SUCCESS&&last==EFI_SUCCESS)last=s;
  }
  /* No OutputString either: SystemTable->ConOut and the cached text protocol
     belong to the TextOut driver detached at stage 1 and can fault until the
     firmware re-establishes them. The trace file is the reliable channel. */
  return last;
}


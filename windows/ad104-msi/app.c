/* SPDX-License-Identifier: GPL-2.0-only */
#include "port.h"
#include "checks.h"
#include "profile.h"
#include "msi_release.h"
#include "boot_io.h"
#include "compat/compat.h"
#include "vendor/unlock_610.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_STATUS Discover(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  EFI_HANDLE *h=NULL,target=NULL;EFI_PCI_IO_PROTOCOL *io=NULL;UINTN count=0,found=0;
  EFI_STATUS s=t->BootServices->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&h);
  if(s==EFI_SUCCESS&&(!h||!count||count>1024))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_PCI_IO_PROTOCOL *p=NULL;UINT32 id=0;
    s=t->BootServices->HandleProtocol(h[i],&PciGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!p||!p->Pci.Read){s=EFI_UNSUPPORTED;break;}
    s=p->Pci.Read(p,EfiPciIoWidthUint32,0,1,&id);
    if(s==EFI_SUCCESS&&id==GU_BOARD_ID){found++;target=h[i];io=p;}
  }
  if(h)t->BootServices->FreePool(h);
  if(s!=EFI_SUCCESS||found!=1||!io)return EFI_UNSUPPORTED;
  GuInit(t->BootServices,io);Gu.image=image;Gu.handle=target;
  Gu.bar_base=GU_BAR0_BASE;Gu.bar_size=0x1000000;
  UINT32 c[64];UINTN seg,bus,dev,fun;GU_CAPS caps;
  if(!io->GetLocation)return EFI_UNSUPPORTED;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s==EFI_SUCCESS)s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s!=EFI_SUCCESS||!GuCheckConfig(c,seg,bus,dev,fun,&caps)||c[2]!=0x030000a1)return EFI_UNSUPPORTED;
  Gu.pm_cap=caps.pm;Gu.pcie_cap=caps.pcie;Gu.pcie_version=caps.pcie_version;Gu.msi_cap=caps.msi;Gu.msix_cap=caps.msix;
  return EFI_SUCCESS;
}
/* Boot trace: writes an ASCII file on our own volume, opened BEFORE and
   independent of GuBootOpen, so an early silent failure still leaves evidence. */
static EFI_FILE_PROTOCOL *TraceFile=NULL;
static VOID Trace(const char *text) {
  if(!TraceFile||!text)return;
  CHAR8 buf[300];UINTN n=0;
  while(text[n]&&n<sizeof(buf)-3){buf[n]=text[n];n++;}
  buf[n++]='\r';buf[n++]='\n';UINTN len=n;
  if(TraceFile->Write(TraceFile,&len,buf)==EFI_SUCCESS)TraceFile->Flush(TraceFile);
}
static VOID TraceHex(const char *tag,UINT64 v) {
  CHAR8 buf[80];UINTN n=0;
  while(tag[n]&&n<40){buf[n]=tag[n];n++;}
  buf[n++]='0';buf[n++]='x';
  for(int i=0;i<16;i++){int d=(int)((v>>((15-i)*4))&0xf);buf[n++]=(CHAR8)(d<10?'0'+d:'a'+d-10);}
  buf[n]=0;Trace(buf);
}
static VOID TraceOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  if(!image||!t||!t->BootServices)return;
  EFI_BOOT_SERVICES *b=t->BootServices;
  EFI_GUID lg=EFI_LOADED_IMAGE_PROTOCOL_GUID,fg=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
  if(EFI_ERROR(b->HandleProtocol(image,&lg,(VOID **)&li))||!li||!li->DeviceHandle){return;}
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  if(EFI_ERROR(b->HandleProtocol(li->DeviceHandle,&fg,(VOID **)&fs))||!fs)return;
  EFI_FILE_PROTOCOL *root=NULL;
  if(EFI_ERROR(fs->OpenVolume(fs,&root))||!root)return;
  EFI_FILE_PROTOCOL *f=NULL;
  if(EFI_ERROR(root->Open(root,&f,L"\\BOOTTRACE.TXT",
      EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))||!f)return;
  f->SetPosition(f,0xFFFFFFFFFFFFFFFFULL);
  TraceFile=f;
  GuBootTraceAttach(f);   /* lets NOTE reach BOOTTRACE after the USB log closes */
}
/* Raw console reporting: works before GuBootOpen and independent of the log
   file, so an early silent failure is still visible on screen. */
static VOID RawPrint(EFI_SYSTEM_TABLE *t,const CHAR16 *m) {
  if(t&&t->ConOut&&t->ConOut->OutputString)t->ConOut->OutputString(t->ConOut,(CHAR16 *)m);
}
static VOID RawHex(EFI_SYSTEM_TABLE *t,UINT64 v,UINTN digits) {
  CHAR16 buf[24];buf[0]=L'0';buf[1]=L'x';
  for(UINTN i=0;i<digits;i++){UINTN sh=(digits-1-i)*4;UINTN d=(UINTN)((v>>sh)&0xf);buf[2+i]=(CHAR16)(d<10?L'0'+d:L'a'+d-10);}
  buf[2+digits]=0;RawPrint(t,buf);
}
static VOID RawWait(EFI_SYSTEM_TABLE *t) {
  RawPrint(t,L"\r\n--- holding 25s so you can read this ---\r\n");
  /* Never depend on ConIn: bootmgr may not hand us an input device at all,
     which is why an earlier version flashed by in a few frames. */
  if(!t||!t->BootServices)return;
  for(UINTN i=0;i<250;i++) {
    if(t->ConIn&&t->ConIn->ReadKeyStroke){EFI_INPUT_KEY k;if(t->ConIn->ReadKeyStroke(t->ConIn,&k)==EFI_SUCCESS)break;}
    t->BootServices->Stall(100000);
  }
}
/* Wall-clock stamps, trace only: by the time the late ones run the console and
   the display driver have been detached, so writing to the screen there would
   either fail silently or touch a dead handle. BOOTTRACE.TXT has its own handle
   and survives GuBootClose, which makes it the one channel that spans the whole
   run. These are measurement only - they change no decision. */
static VOID Stamp(EFI_SYSTEM_TABLE *t,const char *tag) {
  EFI_TIME now;
  if(!t||!t->RuntimeServices||!t->RuntimeServices->GetTime)return;
  if(EFI_ERROR(t->RuntimeServices->GetTime(&now,NULL)))return;
  UINT64 sod=(UINT64)now.Hour*3600ULL+(UINT64)now.Minute*60ULL+(UINT64)now.Second;
  Trace("[T] ");Trace(tag);
  TraceHex(" hms_sec=",sod);TraceHex(" ms3=",(UINT64)(now.Nanosecond/1000000));Trace("\n");
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  TraceOpen(image,t);
  Trace("=== AD104-UNLOCK v0.5.4 ENTRY ===");
  Stamp(t,"ENTRY");
  RawPrint(t,L"\r\n*** AD104-UNLOCK v0.5.4 : ENTRY ***\r\n");
  if(!image||!t||!t->BootServices||!t->RuntimeServices||!t->RuntimeServices->GetVariable){RawPrint(t,L"[F1] bad parameters\r\n");RawWait(t);return EFI_SUCCESS;}
  UINT8 secure=1;UINTN n=1;UINT32 attrs=0;
  EFI_STATUS s=t->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,&attrs,&n,&secure);
  if(s==EFI_SUCCESS){Trace(secure?"[1] SecureBoot=ON refuse":"[1] SecureBoot=OFF okay");RawPrint(t,secure?L"[1] SecureBoot variable = ON -> refusing\r\n":L"[1] SecureBoot variable = OFF\r\n");}
  else {Trace("[1] SecureBoot variable ABSENT -> allow");RawPrint(t,L"[1] SecureBoot variable ABSENT (firmware has no secure boot) -> allowing\r\n");}
  /* Local port: this board's firmware (SetupMode=1) exposes no SecureBoot
     variable at all. EFI_NOT_FOUND there means the firmware reports no signature
     enforcement - treating it as "secure boot active" aborted before any log. */
  if(s==EFI_SUCCESS && (n!=1 || secure)){Trace("[1] SECURE BOOT ON: unlock refused, handing off to bootmgr");RawWait(t);return EFI_SUCCESS;}
  RawPrint(t,L"[2] opening own volume and creating log dir...\r\n");
  s=GuBootOpen(image,t);
  TraceHex("[2] GuBootOpen status=",(UINT64)s);
  RawPrint(t,L"[2] GuBootOpen status=");RawHex(t,s,16);RawPrint(t,L"\r\n");
  /* Local port: never hand the firmware an error status. It abandons the boot
     order and shows a recovery screen without a disk picker. */
  if(s!=EFI_SUCCESS){RawPrint(t,L"[2] FAILED before any log could be written\r\n");RawWait(t);return EFI_SUCCESS;}
  RawPrint(t,L"[2] log dir ready: \\AD104-UNLOCK\\RUNxxxx.TXT\r\n");
  const char *stage="discover";BOOLEAN unlock=FALSE;
  s=Discover(image,t);if(s!=EFI_SUCCESS)goto failed;
  GuLog("AD104 MSI EXPERIMENTAL v0.5.4: hardware unvalidated; USB manual one-shot; bounded USB enumeration additions/removals; GPU ownership pinned\n");
  stage="assets";s=GuBootLoadAssets();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="read-only-cold-profile";s=GuAd104Prepare();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="stage-windows-image";s=GuBootLoadWindows();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="manual-choice";s=GuBootChoose(&unlock);if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  if(!unlock) {
    GuLog("Operator skipped: no GPU transaction, no driver release; stock Windows\n");
    if(Gu.fault||GuBootClose()!=EFI_SUCCESS){Trace("[2b] log close fault; still handing off to bootmgr\n");return EFI_SUCCESS;}
    /* Launching bootmgfw ourselves via StartImage fails: bootmgfw is the Windows
       boot manager, not a plain child application, and returns STATUS_INVALID_
       IMAGE_FORMAT (0xc000007b) which bootmgr then reports as a corrupt image.
       Returning EFI_SUCCESS instead hands control back to the boot manager,
       which continues down its own displayorder and starts Windows normally. */
    s=GuBootWindows();
    Trace("[3] StartImage ");TraceHex("status=",(UINT64)s);
    if(s!=EFI_SUCCESS){Trace("; returning SUCCESS so bootmgr starts Windows itself\n");return EFI_SUCCESS;}
    Trace("\n");
    return EFI_SUCCESS;
  }
  stage="MSI-preflight";s=GuMsiPreflight(t,image,Gu.handle);if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  GuBootSay(L"\r\nChecks passed. Releasing display; watch for Windows.\r\n");
  GuLog("Operator U: one physical attempt; no retries; display callbacks disabled\n");
  if(Gu.fault){s=EFI_DEVICE_ERROR;goto failed;}
  GuBootSilence();
  stage="MSI-release";s=GuMsiRelease();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  if(!GuMsiReleased()||!GuAd104ColdEntry()){s=EFI_NOT_READY;goto failed;}
  Gu.display_check=GuMsiReleased;Gu.display_ok=TRUE;Gu.armed=TRUE;
  UINT64 mask=0xffffffffffffULL;
  struct pci_dev dev={.vendor=0x10de,.device=0x27a0,.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},
    .current_state=PCI_D0,.pm_cap=Gu.pm_cap,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  stage="shared-assets";int result=gpu_unlock_610_load_shared(&dev,&shared);
  if(!result) {stage="firmware-transaction";result=gpu_unlock_610_unlock(&dev,&shared);}
  gpu_unlock_610_release_shared(&shared);
  GuLog("Transaction result=%d safe=%u retained=%u started=%u quiesced=%u flrs=%u DMA_live=%u\n",
    result,GuSafeToReturn(),Gu.retained,Gu.started,Gu.quiesced,Gu.flrs,Gu.dma_count);
  /* Hand the write-time verdict to the final check: it runs later, when the
     device may already be unreachable, and needs to know whether the unlock
     itself was proven rather than guessed. */
  GuMsiSetTransactionResult(result);
  Stamp(t,"UNLOCK-DONE");
  if(result||Gu.fault||!GuSafeToReturn()){s=EFI_DEVICE_ERROR;goto failed;}
  stage="release-exclusive-claim";s=GuMsiFinish();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  /* The final pre-Windows check runs after the log closes: this line is not proof of a Windows start. */
  GuLog("TRANSACTION OK: hardware readbacks passed, DMA released; final pre-Windows check follows unlogged; benchmark still required\n");
  stage="close-USB-log";s=GuBootClose();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  Stamp(t,"FINAL-CHECK");
  stage="final-Windows-verification";if(!GuMsiReadyForWindows()){s=EFI_NOT_READY;goto failed;}
  /* Local port: this branch became reachable for the first time once the final
     check stopped treating an unreachable device as a failure, and it must not
     use StartImage. Calling StartImage on bootmgfw returns
     STATUS_INVALID_IMAGE_FORMAT (0xc000007b) and bootmgr then reports the image
     as corrupt - that is the "file missing or corrupt" screen seen on the earliest
     runs. The containment path already proves the other route works: reconnect
     what we detached, hand the firmware EFI_SUCCESS, and it walks {fwbootmgr}
     on to {bootmgr} and starts Windows itself (RUN0029). Same route here.
     Skip StartImage entirely so this path is identical to the proven one. */
  stage="start-Windows";
  /* No StartImage here (see above). BOOTTRACE, not GuLog: the log was closed at
     close-USB-log, and GuLog would only mark a fault. */
  Trace("[4] handing off to firmware boot order; StartImage skipped\n");
  /* Same reasoning as the containment path: the image returned, so Windows is
     not going to take over the display by itself and the firmware needs its
     drivers back. Reconnect only in this branch - when StartImage succeeded the
     OS owns the GPU and re-initialising it here could undo the unlock. */
  s=GuMsiReconnectDisplay();
  /* Trace goes to BOOTTRACE.TXT through its own handle, so it still works after
     GuBootClose; GuLog cannot be used here (Log is NULL and GuLog would fault). */
  Trace("[4] reconnect status=");TraceHex("",(UINT64)s);Trace("\n");
  /* The log was already closed at the close-USB-log stage, before StartImage, so
     the file is not held open while Windows boots. Only close again if a later
     path reopened it. */
  if(GuBootLogOpen())GuBootClose();
  Stamp(t,"HANDOFF-ok");
  return EFI_SUCCESS;
failed:
  if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  Trace("[STOP] stage=");Trace(stage);TraceHex("      status=",(UINT64)s);
  RawPrint(t,L"\r\n*** STOP at stage=[");
  for(const char *q=stage;q&&*q;q++){CHAR16 c[2]={(CHAR16)*q,0};RawPrint(t,c);}
  RawPrint(t,L"] status=");RawHex(t,s,16);RawPrint(t,L"\r\n");
  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",stage,(unsigned long long)s,GuMsiMutated(),Gu.retained,Gu.fault);
  if(GuMsiMutated()||Gu.started||Gu.retained) {
    /* Fault-independent containment, no freeing of unverified live mappings. */
    if(Gu.started||Gu.master_attributes_pending)(VOID)GuClearMaster();
    /* Containment ran (master cleared, mappings dropped). Parking the CPU here
       only forces a power cycle; Windows re-initialises the GPU from scratch
       anyway, so hand off and let it boot. */
    /* Local port: stages 1-3 detached the TextOut, GOP and NV drivers from the
       GPU and console handles. The MSI machine parked the CPU on failure so the
       user powered off regardless and never noticed the display was gone; here
       we hand off instead, and a firmware with no display driver left on the
       device cannot bring the screen back - the machine just stays black and the
       only exit is a hard power-off, which resets the GPU and throws away the
       whole point of the run. Reconnect what we took out before handing off. */
    s=GuMsiReconnectDisplay();
    Trace("[4b] containment reconnect status=");TraceHex("",(UINT64)s);Trace("\n");
    /* A failure after stage close-USB-log leaves Log==NULL; guard it so GuLog
       does not fail and mark the transaction faulted for nothing. */
    if(GuBootLogOpen())GuLog("containment complete; handing off to firmware boot order\n");
    Trace("containment complete; handing off to firmware boot order\n");
    Stamp(t,"HANDOFF-fail");
    (VOID)GuBootClose();return EFI_SUCCESS;
  }
  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Use Windows Boot Manager.\r\n");
  (VOID)GuBootClose();
  /* Local port: returning the error status here made this firmware abandon the
     remaining boot order instead of falling through to {bootmgr}, which dropped
     the machine into a stripped recovery screen with no disk picker - only
     escapable by power cycling and picking the Windows disk in BIOS. mutated=0
     means no register was written and no driver was released, so handing back
     EFI_SUCCESS is safe: the firmware simply continues to {bootmgr} and Windows
     starts, exactly as it does on the S path. The real status is already in
     BOOTTRACE and the RUN file. */
  return EFI_SUCCESS;
}

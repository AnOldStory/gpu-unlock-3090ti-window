#include "app.h"
#include "app_io.h"
#include "compat/compat.h"
#include "checks.h"
#include "sha256.h"
#include "ownership.h"
#include "vendor/unlock_610.h"
#include "vendor/gfw_eval.h"
#include <Protocol/GraphicsOutput.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/GlobalVariable.h>
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
  PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,GlobalGuid=EFI_GLOBAL_VARIABLE;
static EFI_SYSTEM_TABLE *Table;
static UINT32 InitialConfig[64];
static const char *Stage;
static GU_RELEASE_OWNER PendingOwner;
static EFI_STATUS Step(EFI_STATUS Status) {
  GuLog("CHECK stage=%s status=%#llx\n",Stage,(unsigned long long)Status);
  return !GuLogReady() && !EFI_ERROR(Status)?EFI_DEVICE_ERROR:Status;
}
static VOID ConfigLog(const UINT32 *Config) {
  for(UINTN i=0;i<64;i+=4)GuLog("config[%02x]=%08x %08x %08x %08x\n",
    (unsigned)(i*4),Config[i],Config[i+1],Config[i+2],Config[i+3]);
}
static VOID GopInfoLog(UINTN Index,const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info) {
  GuLog("GOP[%u] format=%u size=%ux%u stride=%u\n",(unsigned)Index,
    (unsigned)Info->PixelFormat,Info->HorizontalResolution,Info->VerticalResolution,Info->PixelsPerScanLine);
}
static BOOLEAN ConsoleGopCheck(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,UINTN Index,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Output) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode=Gop->Mode;
  if(mode->Info && mode->SizeOfInfo>=sizeof(*mode->Info)) {
    *Output=*mode->Info;GopInfoLog(Index,Output);return Output->PixelFormat==PixelBltOnly;
  }
  /* RUN02's console aggregator leaves SizeOfInfo zero. Never read an unsized
     Info pointer: QueryMode supplies its own sized, callee-allocated buffer.
     The caller restricts this fallback to a pathless, zero-framebuffer GOP. */
  if(mode->SizeOfInfo || !Gop->QueryMode)return FALSE;
  UINT32 number=mode->Mode;UINTN bytes=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info=NULL;
  EFI_STATUS status=Gop->QueryMode(Gop,number,&bytes,&info);
  GuLog("GOP[%u] console QueryMode: status=%#llx mode=%u info_bytes=%zu\n",
    (unsigned)Index,(unsigned long long)status,number,(size_t)bytes);
  BOOLEAN valid=!EFI_ERROR(status) && info && bytes>=sizeof(*info);
  if(valid) {
    *Output=*info;GopInfoLog(Index,Output);
    valid=(info->PixelFormat==PixelBltOnly || info->PixelFormat==PixelRedGreenBlueReserved8BitPerColor ||
      info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor) && Gop->Mode==mode && mode->Mode==number &&
      !mode->FrameBufferBase && !mode->FrameBufferSize;
  }
  if(info) {
    status=Gu.bs->FreePool(info);
    if(EFI_ERROR(status)){GuLog("GOP[%u] query FreePool failed: %#llx\n",(unsigned)Index,(unsigned long long)status);valid=FALSE;}
  }
  return valid;
}
static BOOLEAN DisplayCheck(VOID) {
  EFI_HANDLE *handles=NULL;UINTN count=0,intel=0,screens=0;BOOLEAN valid=TRUE,target_screen=FALSE,target_route=FALSE;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION physical={0},forwarded={0};BOOLEAN forwarding=FALSE;
  EFI_STATUS status=Gu.bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  GuLog("GOP enumeration: status=%#llx handles=%zu\n",(unsigned long long)status,(size_t)count);
  if(EFI_ERROR(status) || !handles || !count || count>16){if(handles)GuFree(handles);return FALSE;}
  for(UINTN i=0;i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    status=Gu.bs->HandleProtocol(handles[i],&GopGuid,(VOID **)&gop);
    if(EFI_ERROR(status) || !gop || !gop->Mode){
      GuLog("GOP[%u] invalid protocol/mode: status=%#llx\n",(unsigned)i,
        (unsigned long long)status);valid=FALSE;break;
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode=gop->Mode;
    status=Gu.bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
    int route=!EFI_ERROR(status) && path?GuDisplayPath(path,256):0;
    GuLog("GOP[%u] path_status=%#llx route=%d framebuffer=%#llx bytes=%zu mode=%u max_mode=%u info_bytes=%zu console=%u\n",
      (unsigned)i,(unsigned long long)status,route,(unsigned long long)mode->FrameBufferBase,
      (size_t)mode->FrameBufferSize,mode->Mode,mode->MaxMode,(size_t)mode->SizeOfInfo,(unsigned)(handles[i]==Table->ConsoleOutHandle));
    if((status==EFI_UNSUPPORTED || status==EFI_NOT_FOUND) && !path && !mode->FrameBufferBase &&
       !mode->FrameBufferSize) {
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
      if(ConsoleGopCheck(gop,i,&info)) {
        if(info.PixelFormat==PixelBltOnly)continue;
        /* EDK2 ConSplitter forwards QueryMode to its sole physical GOP. Accept
           that RGB/BGR result only on the system console, then match it against
           the one validated physical GOP after enumeration (handle order varies). */
        if(handles[i]==Table->ConsoleOutHandle && !forwarding) {
          forwarded=info;forwarding=TRUE;continue;
        }
      }
      valid=FALSE;break;
    }
    if(!mode->Info || mode->SizeOfInfo<sizeof(*mode->Info)){valid=FALSE;break;}
    GopInfoLog(i,mode->Info);
    /* The authored configuration required the console on another adapter
       (route 1). The single-GPU profile accepts the target card as the console
       (route 2) and only requires the framebuffer to be sane. */
    if(EFI_ERROR(status) || !path || (!GU_PROFILE_ALLOW_TARGET_DISPLAY && route!=1) ||
       (GU_PROFILE_ALLOW_TARGET_DISPLAY && route==0) || !mode->FrameBufferBase ||
       !mode->FrameBufferSize || mode->Info->PixelFormat>PixelBlueGreenRedReserved8BitPerColor ||
       !mode->Info->HorizontalResolution || !mode->Info->VerticalResolution ||
       mode->Info->PixelsPerScanLine<mode->Info->HorizontalResolution ||
       (UINT64)mode->Info->PixelsPerScanLine*mode->Info->VerticalResolution>mode->FrameBufferSize/4) {
      valid=FALSE;break;
    }
    physical=*mode->Info;screens++;
    if(route==1)intel++;
    if(route==2)target_route=TRUE;
    if(route==2 && handles[i]==Table->ConsoleOutHandle)target_screen=TRUE;
  }
  if(forwarding) {
    BOOLEAN match=(GU_PROFILE_ALLOW_TARGET_DISPLAY?screens==1:intel==1) &&
      forwarded.Version==physical.Version && forwarded.PixelFormat==physical.PixelFormat &&
      forwarded.HorizontalResolution==physical.HorizontalResolution && forwarded.VerticalResolution==physical.VerticalResolution &&
      forwarded.PixelsPerScanLine==physical.PixelsPerScanLine;
    GuLog("GOP console forwarded physical match=%u\n",match);valid=valid && match;
  }
  GuFree(handles);
  GuLog("GOP result: valid=%u intel_physical=%zu screens=%zu console_on_target=%u target_route=%u allow_target_display=%u\n",
    valid,(size_t)intel,(size_t)screens,(unsigned)target_screen,(unsigned)target_route,(unsigned)GU_PROFILE_ALLOW_TARGET_DISPLAY);
  /* On a single-GPU machine the firmware exposes the GPU's own GOP without
     forwarding it through the console handle, so target_screen stays clear even
     though the console really is on the target card. Warn on either signal. */
  if(target_screen || (GU_PROFILE_ALLOW_TARGET_DISPLAY && screens==1 && target_route))
    GuLogText("WARNING: the console display is the target card. The screen will blank between FLR and Windows taking over.\n");
  if(GU_PROFILE_ALLOW_TARGET_DISPLAY)return valid && screens==1 && GuLogReady();
  return valid && intel==1 && GuLogReady();
}
static EFI_STATUS ReadConfig(UINT32 *Config) {
  for(UINTN i=0;i<64;i++)if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&Config[i]))) {
    GuLog("PCI config read failed: offset=%#x valid_bytes=%u; snapshot incomplete\n",(unsigned)(i*4),(unsigned)(i*4));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS ConfigCheck(BOOLEAN Save) {
  UINT32 config[64];GU_CAPS caps;UINTN segment,bus,device,function;
  EFI_STATUS status=Gu.pci->GetLocation(Gu.pci,&segment,&bus,&device,&function);
  GuLog("target location: status=%#llx\n",(unsigned long long)status);
  if(EFI_ERROR(status))return status;
  status=ReadConfig(config);if(EFI_ERROR(status))return status;
  BOOLEAN checked=GuCheckConfig(config,segment,bus,device,function,&caps);
  BOOLEAN classrev=(config[2]&0xffffff00U)==0x03000000U && (config[2]&0xffU)==GU_BOARD_REVISION;
  BOOLEAN c8ok=(GU_BOARD_CFG_C8==GU_BOARD_CFG_C8_ANY)?TRUE:(config[0xc8/4]==GU_BOARD_CFG_C8);
  BOOLEAN barok=(GU_BOARD_BAR0_BASE==GU_BOARD_BAR0_ANY)?TRUE:(config[0x10/4]==GU_BOARD_BAR0_BASE);
  BOOLEAN valid=checked && classrev && c8ok && barok &&
    !(config[(caps.pcie+8)/4]&0x00200000U);
  BOOLEAN same=Save || !memcmp(config,InitialConfig,sizeof(config));
  if(Save || !valid || !same)ConfigLog(config);
  GuLog("target BDF=%zu:%zu:%zu.%zu checks=%u classrev=%u c8=%u bar=%u pm=%#x pcie=%#x msi=%#x msix=%#x\n",
    (size_t)segment,(size_t)bus,(size_t)device,(size_t)function,checked,
    classrev,c8ok,barok,
    (unsigned)caps.pm,(unsigned)caps.pcie,(unsigned)caps.msi,(unsigned)caps.msix);
  if(!valid)return EFI_UNSUPPORTED;
  if(!same)return EFI_NOT_READY;
  if(Save)memcpy(InitialConfig,config,sizeof(config));
  Gu.pm_cap=caps.pm;Gu.pcie_cap=caps.pcie;Gu.pcie_version=caps.pcie_version;Gu.msi_cap=caps.msi;Gu.msix_cap=caps.msix;
  return EFI_SUCCESS;
}
static EFI_STATUS RestoreDecode(VOID) {
  UINT32 current[64],normalized[64],after[64];UINTN segment,bus,device,function;
  if(!Gu.pci || !Gu.owned || !Gu.armed || !Gu.display_ok || Gu.started || Gu.allocations ||
    Gu.dma_count || Gu.fault || Gu.retained)return EFI_ACCESS_DENIED;
  EFI_STATUS status=Gu.pci->GetLocation(Gu.pci,&segment,&bus,&device,&function);
  if(EFI_ERROR(status))return status;
  status=ReadConfig(current);if(EFI_ERROR(status))return status;
  GuLogText("config before decode restore (256 valid bytes):\n");ConfigLog(current);
  if(!GuTargetConfig(current,segment,bus,device,function))return EFI_UNSUPPORTED;
  UINT32 missing=InitialConfig[1]&~current[1]&3U;
  memcpy(normalized,current,sizeof(normalized));normalized[1]|=missing;
  /* Restore only cleared, previously enabled IO/MEM bits. Every other bit,
     including BME, status, BAR, ROM and PM/capability state must be unchanged. */
  if(memcmp(normalized,InitialConfig,sizeof(normalized))) {
    GuLogText("decode restore refused: config differs beyond cleared original IO/MEM bits\n");
    return EFI_UNSUPPORTED;
  }
  if(!missing){GuLogText("decode restore not needed\n");return EFI_SUCCESS;}
  if(!Gu.pci->Attributes)return EFI_UNSUPPORTED;
  UINT64 needed=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  UINT64 supported=0,attributes=0;
  status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationSupported,0,&supported);
  GuLog("decode supported attributes: status=%#llx mask=%#llx\n",(unsigned long long)status,(unsigned long long)supported);
  if(EFI_ERROR(status))return status;
  if((supported&needed)!=needed)return EFI_UNSUPPORTED;
  status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attributes);
  GuLog("decode current attributes: status=%#llx mask=%#llx\n",(unsigned long long)status,(unsigned long long)attributes);
  if(EFI_ERROR(status))return status;
  GuLog("decode restore intent: saved_command=%#x current_command=%#x missing=%#x attributes=%#llx\n",
    InitialConfig[1]&0xffff,current[1]&0xffff,missing,(unsigned long long)needed);
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  /* Never use EFI_PCI_DEVICE_ENABLE: it would also enable bus mastering. */
  EFI_STATUS enabled=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationEnable,needed,NULL);
  GuLog("decode enable status=%#llx\n",(unsigned long long)enabled);
  if(!GuLogReady() || Gu.fault)return EFI_ERROR(enabled)?enabled:EFI_DEVICE_ERROR;
  /* Even an error may follow a partial hardware effect. Capture it if readable,
     but never turn an API error into success merely because readback matches. */
  status=ReadConfig(after);
  BOOLEAN matched=FALSE;
  if(!EFI_ERROR(status)) {
    GuLogText("config after decode restore (256 valid bytes):\n");ConfigLog(after);
    matched=!memcmp(after,InitialConfig,sizeof(after));GuLog("decode readback matched=%u\n",matched);
  }
  if(EFI_ERROR(enabled))return enabled;
  if(EFI_ERROR(status))return status;
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  return matched?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}
static EFI_STATUS FindTarget(VOID) {
  EFI_HANDLE *handles=NULL;UINTN count=0,found=0;
  EFI_STATUS status=Gu.bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  GuLog("PCI enumeration: status=%#llx handles=%zu\n",(unsigned long long)status,(size_t)count);
  if(EFI_ERROR(status) || !handles || !count || count>256){if(handles)GuFree(handles);return EFI_NOT_FOUND;}
  for(UINTN i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *pci=NULL;UINT32 id=0;
    status=Gu.bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&pci);
    if(!EFI_ERROR(status) && pci)status=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,1,&id);
    if(EFI_ERROR(status) || !pci){GuLog("PCI scan failed: index=%zu status=%#llx\n",(size_t)i,(unsigned long long)status);status=EFI_DEVICE_ERROR;break;}
    if(id==GU_BOARD_ID){found++;Gu.handle=handles[i];Gu.pci=pci;}
  }
  GuFree(handles);GuLog("PCI target matches=%zu status=%#llx\n",(size_t)found,(unsigned long long)status);
  if(EFI_ERROR(status) || found!=1)return EFI_UNSUPPORTED;
  return ConfigCheck(TRUE);
}
static EFI_STATUS BarCheck(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *r=NULL;
  EFI_STATUS status=Gu.pci->GetBarAttributes(Gu.pci,0,NULL,(VOID **)&r);
  GuLog("bar0 query: status=%#llx\n",(unsigned long long)status);
  if(!EFI_ERROR(status)) {
    if(r && r->Desc==ACPI_ADDRESS_SPACE_DESCRIPTOR && r->Len==sizeof(*r)-3)
      GuLog("bar0 descriptor: type=%u granularity=%llu min=%#llx max=%#llx length=%#llx translation=%#llx\n",
        (unsigned)r->ResType,(unsigned long long)r->AddrSpaceGranularity,(unsigned long long)r->AddrRangeMin,
        (unsigned long long)r->AddrRangeMax,(unsigned long long)r->AddrLen,(unsigned long long)r->AddrTranslationOffset);
    if(!r || r->Desc!=ACPI_ADDRESS_SPACE_DESCRIPTOR || r->Len!=sizeof(*r)-3 || r->ResType!=0 ||
      /* A pinned board address is optional; the descriptor shape is not. */
      (GU_BOARD_BAR0_BASE!=GU_BOARD_BAR0_ANY && r->AddrRangeMin!=GU_BOARD_BAR0_BASE) ||
      r->AddrLen!=GU_BOARD_BAR0_SIZE || r->AddrTranslationOffset ||
      /* UEFI describes an inclusive end; pinned EDK2 PciBusDxe instead returns
         PciBar.Alignment. Both representations describe this exact BAR. */
      !((r->AddrRangeMax==r->AddrRangeMin+r->AddrLen-1) ||
        (r->AddrRangeMax==r->AddrLen-1)) ||
      r->AddrSpaceGranularity!=32 || ((UINT8 *)(r+1))[0]!=ACPI_END_TAG_DESCRIPTOR)status=EFI_UNSUPPORTED;
    else {
      UINT8 *raw=(UINT8 *)r,sum=0;
      for(UINTN i=0;i<sizeof(*r)+2;i++)sum=(UINT8)(sum+raw[i]);
      if(raw[sizeof(*r)+1] && sum)status=EFI_COMPROMISED_DATA;
      else {Gu.bar_base=r->AddrRangeMin;Gu.bar_size=r->AddrLen;}
    }
  }
  if(r)GuFree(r);return status;
}
static BOOLEAN WarmBoot;
static EFI_STATUS ColdCheck(VOID) {
  UINT32 chip=GuRead32(0),plm=GuRead32(GU_FEAT_PLM_OFFSET),ss0=GuRead32(GU_FEAT_SS0_OFFSET),
    ss1=GuRead32(GU_FEAT_SS1_OFFSET),cold_1183a4=GuRead32(0x1183a4),hi=GuRead32(0x1fa828);
  BOOLEAN accessible=(ss0!=0xffffffffU && ss1!=0xffffffffU);
  BOOLEAN observed=(ss0==GU_BOARD_COLD_SS0 && ss1==GU_BOARD_COLD_SS1 && cold_1183a4==GU_BOARD_COLD_1183A4);
  GuLog("cold state: chip=%#x plm=%#x SS0=%#x SS1=%#x 1183a4=%#x wpr2_hi=%#x\n",
    chip,plm,ss0,ss1,cold_1183a4,hi);
  GuLog("chip: actual=%#x expected=%#x\n",chip,GU_BOARD_CHIP);
  if(chip!=GU_BOARD_CHIP)return EFI_UNSUPPORTED;
  struct gpu_unlock_gfw_sample s={.config_ok=true,.plm=GuRead32(GU_GFW_PLM_OFFSET),
    .boot=GuRead32(GU_GFW_BOOT_OFFSET),.cpuctl=GuRead32(GU_GFW_CPUCTL_OFFSET)};
  GuLog("cold expected: plm=%#x SS0=%#x SS1=%#x 1183a4=%#x wpr2_hi<=%#x strict=%u observed=%u accessible=%u\n",
    GU_BOARD_COLD_PLM,GU_BOARD_COLD_SS0,GU_BOARD_COLD_SS1,GU_BOARD_COLD_1183A4,GU_BOARD_COLD_WPR2_MAX,
    (unsigned)GU_BOARD_COLD_STRICT,(unsigned)observed,(unsigned)accessible);
  /* SS0/SS1 are gated on readability, exactly as the validated 3090 Ti Linux
     module does (this card's SS1 reads 6, the authored 3080 saw 2 or 3), and the
     written values are verified again after the selector writes. Set
     GU_BOARD_COLD_STRICT to demand the measured values instead. */
  if(Gu.fault || gpu_unlock_gfw_assess(&s) || hi>GU_BOARD_COLD_WPR2_MAX || plm!=GU_BOARD_COLD_PLM ||
    !accessible || (GU_BOARD_COLD_STRICT && !observed)) {
    /* WPR2 already programmed means the card was initialised in this power cycle:
       a warm reboot, not a cold boot. Nothing has been touched at this point. */
    if(hi>GU_BOARD_COLD_WPR2_MAX) {
      WarmBoot=TRUE;
      GuLogText("cold state: WPR2 is already set, so this is a warm boot; the unlock needs a full power-off.\n");
    }
    return EFI_NOT_READY;
  }
  return Gu.fault?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static VOID DigestLog(const UINT8 Digest[32]) {
  static const char Digits[]="0123456789abcdef";char Hex[65];
  for(UINTN i=0;i<32;i++){Hex[i*2]=Digits[Digest[i]>>4];Hex[i*2+1]=Digits[Digest[i]&15];}
  Hex[64]=0;GuLog("live PROM sha256=%s\n",Hex);
}
static EFI_STATUS PromCheck(VOID) {
  VOID *prom=GuAlloc(1048576);if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  UINT8 digest[32];GuSha256(prom,1048576,digest);DigestLog(digest);
  if(GU_BOARD_PROM_SHA256[0]==0) {
    GuLogText("board ROM profile is unset: paste the sha256 above into GU_BOARD_PROM_SHA256 and rebuild.\n");
    GuFree(prom);return Gu.fault?EFI_DEVICE_ERROR:EFI_CRC_ERROR;
  }
  BOOLEAN good=!Gu.fault && GuHashMatches(prom,1048576,GU_BOARD_PROM_SHA256);
  GuFree(prom);GuLog("live PROM hash matched=%u expected=%s\n",(unsigned)good,GU_BOARD_PROM_SHA256);
  return good?EFI_SUCCESS:EFI_CRC_ERROR;
}
static BOOLEAN ArmedManually;
static EFI_STATUS Arm(VOID) {
  if(!Table->ConIn || !Table->ConIn->Reset || !Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  EFI_STATUS status=Table->ConIn->Reset(Table->ConIn,FALSE);if(EFI_ERROR(status))return status;
  GuLogText("READY: " GU_PROFILE_NAME " / single-GPU profile / verified boot-volume log and firmware.\n");
  GuLogText("Console is the target card: expect a blank screen between FLR and Windows taking over.\n");
  if(PendingOwner.agent)GuLogText("The start releases the RTX firmware driver, restores verified access settings if needed, then rechecks and attempts once.\n");
  if(PendingOwner.agent && GU_PROFILE_RELEASE_CONSOLE_DRIVER)
    GuLogText("That release removes the console display driver, so this screen stops updating: the report continues on the boot volume.\n");
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  /* Automatic mode: the entry runs on every boot, so the cold boot is the
     confirmation and any key during the countdown is the way out. A marker left by
     an attempt that did not finish keeps automatic start off for this boot. */
  BOOLEAN manual=GU_PROFILE_AUTO_ARM && GuMarkFailedRead();
  if(manual)GuLogText("A previous attempt did not finish: automatic start is off for this boot.\n");
  if(GU_PROFILE_AUTO_ARM && !manual) {
    for(UINTN left=GU_PROFILE_AUTO_ARM_SECONDS;left>0;left--) {
      GuLog("AUTO: starting in %u s; press any key to boot Windows without unlocking.\n",(unsigned)left);
      if(!GuLogReady())return EFI_DEVICE_ERROR;
      for(UINTN tick=0;tick<10;tick++) {
        EFI_INPUT_KEY key;status=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
        if(!EFI_ERROR(status)) {GuLogText("AUTO: skipped by key press; nothing was changed.\n");return EFI_ABORTED;}
        if(status!=EFI_NOT_READY)return status;
        GuStall(100000);
      }
    }
    return EFI_SUCCESS;
  }
  GuLogText("Press U to attempt once; Esc or 30-second timeout cancels.\n");
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<300;i++) {
    EFI_INPUT_KEY key;status=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(!EFI_ERROR(status)) {
      if(key.ScanCode==0x17 || key.UnicodeChar==27)return EFI_ABORTED;
      if(key.UnicodeChar=='u' || key.UnicodeChar=='U'){ArmedManually=TRUE;return EFI_SUCCESS;}
    } else if(status!=EFI_NOT_READY)return status;
    GuStall(100000);
  }
  return EFI_TIMEOUT;
}
static VOID Halt(VOID) {
  Gu.retained=TRUE;
  GuLogText("STOP: cleanup cannot be proven. Resources remain reserved.\nPOWER OFF using the physical power button; do not continue to Windows.\n");
  /* No ResetSystem, ExitBootServices, retry, protocol close or memory release. */
  for(;;)Gu.bs->Stall(1000000);
}
static VOID LogContainment(VOID) {
  /* Called only after any TPL_NOTIFY containment section has ended. */
  GuLog("BME containment: attribute_status=%#llx hardware_status=%#llx command=%#x cached=%#llx pending=%u\n",
    (unsigned long long)Gu.master_attribute_status,(unsigned long long)Gu.master_hardware_status,
    Gu.master_last_command,(unsigned long long)Gu.master_last_attributes,Gu.master_attributes_pending);
}
static VOID ReleaseHalt(EFI_STATUS Status,BOOLEAN Attempted) {
  /* A failed claim may follow interface removal/replacement during logging.
     Resolve, validate and clear BME without a callback window or stale access. */
  EFI_TPL old=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS refreshed=GuRefreshTargetPci();
  EFI_STATUS contained=EFI_ERROR(refreshed)?refreshed:GuClearMaster();
  Gu.bs->RestoreTPL(old);
  LogContainment();
  GuLog("FINAL status=%#llx stage=%s attempted=%u driver_release=1 clean=0 containment=%#llx pci_available=%u\n",
    (unsigned long long)Status,Stage,Attempted,(unsigned long long)contained,(unsigned)(Gu.pci!=NULL));
  Halt();
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(!St || !St->BootServices || !St->RuntimeServices)return EFI_INVALID_PARAMETER;
  Table=St;GuInit(St->BootServices,NULL);Gu.image=Image;Gu.display_check=DisplayCheck;
  memset(&PendingOwner,0,sizeof(PendingOwner));
  Stage="boot-volume-log";EFI_STATUS status=GuIoOpen(Image,St);BOOLEAN attempted=FALSE,release_attempted=FALSE;
  if(EFI_ERROR(status))goto finish;
  UINT8 secure=0xff;UINTN bytes=1;
  Stage="secure-boot";
  status=St->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,NULL,&bytes,&secure);
  if(EFI_ERROR(status) || bytes!=1 || secure){status=EFI_SECURITY_VIOLATION;goto finish;}
  status=Step(status);if(EFI_ERROR(status))goto finish;
  Stage="watchdog";status=Step(Gu.bs->SetWatchdogTimer(0,0,0,NULL));if(EFI_ERROR(status))goto finish;
  Stage="target";status=Step(FindTarget());if(EFI_ERROR(status))goto finish;
  Stage="display";Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);if(EFI_ERROR(status))goto finish;
  Stage="ownership";status=GuClaimController();
  if(status==EFI_ACCESS_DENIED) {
    status=GuFindReleaseOwner(St,&PendingOwner,TRUE);
    if(!EFI_ERROR(status))GuLogText("Exclusive ownership deferred: the driver is released only after the start is confirmed.\n");
  }
  status=Step(status);if(EFI_ERROR(status))goto finish;
  Stage="config-recheck";status=Step(ConfigCheck(FALSE));if(EFI_ERROR(status))goto finish;
  Stage="bar0";status=Step(BarCheck());if(EFI_ERROR(status))goto finish;
  Stage="cold-state";status=Step(ColdCheck());if(EFI_ERROR(status))goto finish;
  Stage="prom";status=Step(PromCheck());if(EFI_ERROR(status))goto finish;
  Stage="assets";status=Step(GuLoadAssets());if(EFI_ERROR(status))goto finish;
  Stage="arming";status=Step(Arm());if(EFI_ERROR(status))goto finish;
  Stage="armed-config";status=Step(ConfigCheck(FALSE));if(EFI_ERROR(status))goto finish;
  Stage="armed-display";Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);if(EFI_ERROR(status))goto finish;
  Stage="armed-cold-state";status=Step(ColdCheck());if(EFI_ERROR(status))goto finish;
  if(PendingOwner.agent) {
    GU_RELEASE_OWNER current;
    Stage="owner-recheck";
    GuLogText(ArmedManually?"Manual U confirmed: release RTX firmware driver once, then revalidate.\n"
      :"Start confirmed: release RTX firmware driver once, then revalidate.\n");
    if(!GuLogReady()){status=EFI_DEVICE_ERROR;goto finish;}
    /* Durable intent precedes the last owner snapshot. No file IO between a
       successful final comparison and DisconnectController. */
    status=GuFindReleaseOwner(St,&current,FALSE);
    if(!EFI_ERROR(status) && !GuSameReleaseOwner(&PendingOwner,&current))status=EFI_ABORTED;
    if(EFI_ERROR(status)){status=Step(status);goto finish;}
    if(GU_PROFILE_RELEASE_CONSOLE_DRIVER) {
      /* The driver about to be released produces this console's output, so text
         stops here. Mute before the call: a dead console must not be recorded as
         a logging fault, and the report stays the authoritative record. */
      GuLogText("Screen output ends now: releasing the console display driver.\n");
      if(!GuLogReady()){status=EFI_DEVICE_ERROR;goto finish;}
      GuMuteConsole();Gu.console_released=TRUE;
    }
    EFI_PCI_IO_PROTOCOL *prior=Gu.pci;
    Stage="driver-release";release_attempted=TRUE;Gu.armed=TRUE;Gu.pci=NULL;
    EFI_STATUS released=Gu.bs->DisconnectController(Gu.handle,current.agent,NULL);
    EFI_TPL refresh_tpl=Gu.bs->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS refreshed=GuRefreshTargetPci();
    Gu.bs->RestoreTPL(refresh_tpl);
    GuLog("driver release: status=%#llx refresh=%#llx same_pci=%u\n",
      (unsigned long long)released,(unsigned long long)refreshed,(unsigned)(Gu.pci==prior));
    status=Step(EFI_ERROR(released)?released:EFI_ERROR(refreshed)?refreshed:Gu.pci!=prior?EFI_COMPROMISED_DATA:EFI_SUCCESS);
    if(EFI_ERROR(status))goto finish;
    Stage="ownership-after-release";status=Step(GuClaimController());if(EFI_ERROR(status))goto finish;
    Stage="display-after-release";
    if(GU_PROFILE_RELEASE_CONSOLE_DRIVER) {
      /* The released owner was the console display driver, so the GOP and its
         text output are gone by design and there is nothing left to revalidate.
         The unlock itself only needs the card, not a display. */
      GuLogText("display-after-release skipped: the released driver was the console\n");
      status=Step(EFI_SUCCESS);
    } else {
      Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);
    }
    if(EFI_ERROR(status))goto finish;
    Stage="decode-after-release";status=Step(RestoreDecode());if(EFI_ERROR(status))goto finish;
    Stage="config-after-release";status=Step(ConfigCheck(FALSE));if(EFI_ERROR(status))goto finish;
    Stage="bar0-after-release";status=Step(BarCheck());if(EFI_ERROR(status))goto finish;
    Stage="cold-after-release";status=Step(ColdCheck());if(EFI_ERROR(status))goto finish;
    Stage="prom-after-release";status=Step(PromCheck());if(EFI_ERROR(status))goto finish;
  }
  GuLogText("ARMED: one transaction is starting. Do not reset or remove the boot volume.\n");
  if(!GuLogReady()){status=EFI_DEVICE_ERROR;goto finish;}
  /* Durable intent before any register access: if this attempt never reaches its
     end, the next boot asks for an explicit U instead of retrying unattended. */
  Stage="attempt-marker";status=Step(GuMarkFailedWrite(TRUE));if(EFI_ERROR(status))goto finish;
  Stage="transaction";Gu.armed=TRUE;attempted=TRUE;
  UINT64 mask=0xffffffffULL;
  struct pci_dev device={.dev={.dma_mask=&mask,.coherent_dma_mask=0xffffffffULL},.pm_cap=Gu.pm_cap,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  int result=gpu_unlock_610_load_shared(&device,&shared);
  if(!result)result=gpu_unlock_610_unlock(&device,&shared);
  /* Also covers failures before GuStarted, including BME-enable readback. */
  EFI_STATUS contained=GuClearMaster();
  LogContainment();
  GuLog("transaction result=%d containment=%#llx started=%u quiesced=%u fault=%u retained=%u maps=%u FLR=%u\n",
    result,(unsigned long long)contained,Gu.started,Gu.quiesced,Gu.fault,Gu.retained,Gu.dma_count,Gu.flrs);
  if(EFI_ERROR(contained) || !GuSafeToReturn() || Gu.allocations || (Gu.started && result))Halt();
  gpu_unlock_610_release_shared(&shared);Gu.armed=FALSE;
  status=result?EFI_DEVICE_ERROR:EFI_SUCCESS;
  if(!result) {
    /* Success is the only way back to unattended start. */
    EFI_STATUS cleared=GuMarkFailedWrite(FALSE);
    GuLog("attempt marker cleared=%u\n",(unsigned)!EFI_ERROR(cleared));
  }
  if(!EFI_ERROR(status) && Gu.console_released) {
    /* RUN01 reached the end of the transaction and then never handed off: the
       firmware boot menu and Windows Boot Manager both need a graphics console,
       and the display driver was released for the FLR. Reconnect it so the
       handoff looks like a normal boot, then read the selectors back to prove
       the unlock survived that driver's initialisation. */
    Stage="display-restore";
    if(Gu.owned) {
      EFI_STATUS closed=Gu.bs->CloseProtocol(Gu.handle,&PciGuid,Image,NULL);
      if(!EFI_ERROR(closed))Gu.owned=FALSE;
    }
    EFI_STATUS connected=Gu.bs->ConnectController?Gu.bs->ConnectController(Gu.handle,NULL,NULL,TRUE):EFI_UNSUPPORTED;
    GuLog("display restore: connect=%#llx owned=%u\n",(unsigned long long)connected,(unsigned)Gu.owned);
    if(!EFI_ERROR(connected) && !EFI_ERROR(GuRefreshTargetPci())) {
      GuLog("post-restore selectors: SS0=%#x SS1=%#x (unlock wrote 0x88888888/0x8)\n",
        GuRead32(GU_FEAT_SS0_OFFSET),GuRead32(GU_FEAT_SS1_OFFSET));
      GuUnmuteConsole();Gu.display_ok=DisplayCheck();
      if(Gu.display_ok)GuLogText("Display restored: the released driver reconnected, screen output resumes.\n");
      else GuLog("display restore incomplete: console unusable, handoff continues without it\n");
    } else {
      GuLog("display restore failed: handoff continues without a console\n");
    }
  }
finish:
  if(release_attempted && (EFI_ERROR(status) || Gu.fault))ReleaseHalt(status,attempted);
  if(attempted && !GuSafeToReturn())Halt();
  if(GuLogReady())GuLog("FINAL status=%#llx stage=%s attempted=%u driver_release=%u clean=%u\n",(unsigned long long)status,Stage,attempted,release_attempted,GuSafeToReturn());
  else GuConsole("Preflight or logging failed; no new attempt will start.\n");
  if(!GuLogReady()){status=EFI_DEVICE_ERROR;if(release_attempted)ReleaseHalt(status,attempted);if(Gu.started)Halt();}
  if(attempted && !GuSafeToReturn())Halt();
  EFI_STATUS closed=GuIoClose();
  if(EFI_ERROR(closed)){if(release_attempted)ReleaseHalt(closed,attempted);if(Gu.started)Halt();status=closed;}
  if(Gu.owned) {
    closed=Gu.bs->CloseProtocol(Gu.handle,&PciGuid,Image,NULL);
    if(EFI_ERROR(closed)){if(release_attempted)ReleaseHalt(closed,attempted);if(Gu.started)Halt();status=closed;}else Gu.owned=FALSE;
  }
  GuReleaseAssets();
  if(!EFI_ERROR(status))GuConsole("UEFI transaction completed and cleaned up. Returning to firmware; Windows validation is pending.\n");
  else if(WarmBoot)GuConsole("Not a cold boot; nothing was changed. Continuing to Windows.\n");
  else GuConsole("Attempt cancelled or rejected. Returning without another attempt.\n");
  return status;
}

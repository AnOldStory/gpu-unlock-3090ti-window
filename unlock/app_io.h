#pragma once
#include "port.h"
EFI_STATUS GuIoOpen(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table);
EFI_STATUS GuIoClose(VOID);
EFI_STATUS GuLoadAssets(VOID);
VOID GuReleaseAssets(VOID);
BOOLEAN GuLogReady(VOID);
VOID GuConsole(const char *Text);
VOID GuMuteConsole(VOID);
VOID GuUnmuteConsole(VOID);
/* Automatic mode marker: an attempt that did not finish leaves UNFINISHED.TXT on
   the payload volume, so the next boot asks for an explicit U instead of retrying
   unattended. Read is a plain existence check; write(TRUE) creates it, write(FALSE)
   deletes it. */
BOOLEAN GuMarkFailedRead(VOID);
EFI_STATUS GuMarkFailedWrite(BOOLEAN Present);

/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuMsiRelease(VOID);
BOOLEAN GuMsiReleased(VOID);
EFI_STATUS GuMsiFinish(VOID);
EFI_STATUS GuMsiReconnectDisplay(VOID);
BOOLEAN GuMsiReadyForWindows(VOID);
BOOLEAN GuMsiMutated(VOID);
VOID GuMsiSetTransactionResult(int);
BOOLEAN GuMsiRegisterVerdict(BOOLEAN,UINT32,UINT32,UINT32,UINT32,UINT32,BOOLEAN,BOOLEAN);

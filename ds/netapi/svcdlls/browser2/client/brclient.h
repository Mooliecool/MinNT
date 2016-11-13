/*++

Copyright (c) 1991 Microsoft Corporation

Module Name:

    brclient.h

Abstract:

    Private header file for the client end of the Browser service
    modules.

Author:

    Rita Wong (ritaw) 10-May-1991

Revision History:

--*/

#include <nt.h>                  // DbgPrint prototype
#include <ntrtl.h>                  // DbgPrint
#include <nturtl.h>                 // Needed by winbase.h

#include <windef.h>                 // DWORD
#include <winbase.h>                // LocalFree

#include <rpc.h>                    // DataTypes and runtime APIs
#include <rpcutil.h>                // GENERIC_ENUM_STRUCT

#include <lmcons.h>                 // NET_API_STATUS
#include <lmerr.h>                  // NetError codes
#include <lmremutl.h>               // SUPPORTS_RPC

#include <netlibnt.h>               // NetpNtStatusToApiStatus
#include <netdebug.h>               // NetpDbgPrint

#include <bowser.h>                 // generated by the MIDL complier
#include <brnames.h>                // Service and interface names

//
// Debug trace level bits for turning on/off trace statements in the client
// end of the Browser service
//

//
// Client stub trace output
//
#define BROWSER_DEBUG_CLIENTSTUBS    0x00000001

//
// Client RPC binding trace output
//
#define BROWSER_DEBUG_RPCBIND        0x00000002

//
// All debug flags on
//
#define BROWSER_DEBUG_ALL            0xFFFFFFFF


#if DBG

#define STATIC

#else

#define STATIC static

#endif // DBG

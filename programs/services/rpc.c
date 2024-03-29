/*
 * Services.exe - RPC functions
 *
 * Copyright 2007 Google (Mikolaj Zalewski)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define WIN32_LEAN_AND_MEAN
#define NONAMELESSUNION

#include <stdarg.h>
#include <windows.h>
#include <winternl.h>
#include <winsvc.h>
#include <ntsecapi.h>
#include <rpc.h>

#include "wine/list.h"
#include "wine/unicode.h"
#include "wine/debug.h"

#include "services.h"
#include "svcctl.h"

extern HANDLE CDECL __wine_make_process_system(void);

WINE_DEFAULT_DEBUG_CHANNEL(service);

static const GENERIC_MAPPING g_scm_generic =
{
    (STANDARD_RIGHTS_READ | SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_QUERY_LOCK_STATUS),
    (STANDARD_RIGHTS_WRITE | SC_MANAGER_CREATE_SERVICE | SC_MANAGER_MODIFY_BOOT_CONFIG),
    (STANDARD_RIGHTS_EXECUTE | SC_MANAGER_CONNECT | SC_MANAGER_LOCK),
    SC_MANAGER_ALL_ACCESS
};

static const GENERIC_MAPPING g_svc_generic =
{
    (STANDARD_RIGHTS_READ | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS),
    (STANDARD_RIGHTS_WRITE | SERVICE_CHANGE_CONFIG),
    (STANDARD_RIGHTS_EXECUTE | SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL),
    SERVICE_ALL_ACCESS
};

typedef enum
{
    SC_HTYPE_DONT_CARE = 0,
    SC_HTYPE_MANAGER,
    SC_HTYPE_SERVICE
} SC_HANDLE_TYPE;

struct sc_handle
{
    SC_HANDLE_TYPE type;
    DWORD access;
};

struct sc_manager_handle       /* service control manager handle */
{
    struct sc_handle hdr;
    struct scmdatabase *db;
};

struct sc_service_handle       /* service handle */
{
    struct sc_handle hdr;
    struct service_entry *service_entry;
};

struct sc_lock
{
    struct scmdatabase *db;
};

static HANDLE timeout_queue_event;
static CRITICAL_SECTION timeout_queue_cs;
static CRITICAL_SECTION_DEBUG timeout_queue_cs_debug =
{
    0, 0, &timeout_queue_cs,
    { &timeout_queue_cs_debug.ProcessLocksList, &timeout_queue_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": timeout_queue_cs") }
};
static CRITICAL_SECTION timeout_queue_cs = { &timeout_queue_cs_debug, -1, 0, 0, 0, 0 };
static struct list timeout_queue = LIST_INIT(timeout_queue);
struct timeout_queue_elem
{
    struct list entry;

    FILETIME time;
    struct process_entry *process;
};

static void terminate_after_timeout(struct process_entry *process, DWORD timeout)
{
    struct timeout_queue_elem *elem;
    ULARGE_INTEGER time;

    if (!(elem = HeapAlloc(GetProcessHeap(), 0, sizeof(*elem))))
        return;

    elem->process = grab_process(process);

    GetSystemTimeAsFileTime(&elem->time);
    time.u.LowPart = elem->time.dwLowDateTime;
    time.u.HighPart = elem->time.dwHighDateTime;
    time.QuadPart += (ULONGLONG)timeout * 10000;
    elem->time.dwLowDateTime = time.u.LowPart;
    elem->time.dwHighDateTime = time.u.HighPart;

    EnterCriticalSection(&timeout_queue_cs);
    list_add_head(&timeout_queue, &elem->entry);
    LeaveCriticalSection(&timeout_queue_cs);

    SetEvent(timeout_queue_event);
}

static void free_service_strings(struct service_entry *old, struct service_entry *new)
{
    QUERY_SERVICE_CONFIGW *old_cfg = &old->config;
    QUERY_SERVICE_CONFIGW *new_cfg = &new->config;

    if (old_cfg->lpBinaryPathName != new_cfg->lpBinaryPathName)
        HeapFree(GetProcessHeap(), 0, old_cfg->lpBinaryPathName);

    if (old_cfg->lpLoadOrderGroup != new_cfg->lpLoadOrderGroup)
        HeapFree(GetProcessHeap(), 0, old_cfg->lpLoadOrderGroup);

    if (old_cfg->lpServiceStartName != new_cfg->lpServiceStartName)
        HeapFree(GetProcessHeap(), 0, old_cfg->lpServiceStartName);

    if (old_cfg->lpDisplayName != new_cfg->lpDisplayName)
        HeapFree(GetProcessHeap(), 0, old_cfg->lpDisplayName);

    if (old->dependOnServices != new->dependOnServices)
        HeapFree(GetProcessHeap(), 0, old->dependOnServices);

    if (old->dependOnGroups != new->dependOnGroups)
        HeapFree(GetProcessHeap(), 0, old->dependOnGroups);
}

/* Check if the given handle is of the required type and allows the requested access. */
static DWORD validate_context_handle(SC_RPC_HANDLE handle, DWORD type, DWORD needed_access, struct sc_handle **out_hdr)
{
    struct sc_handle *hdr = handle;

    if (type != SC_HTYPE_DONT_CARE && hdr->type != type)
    {
        WINE_ERR("Handle is of an invalid type (%d, %d)\n", hdr->type, type);
        return ERROR_INVALID_HANDLE;
    }

    if ((needed_access & hdr->access) != needed_access)
    {
        WINE_ERR("Access denied - handle created with access %x, needed %x\n", hdr->access, needed_access);
        return ERROR_ACCESS_DENIED;
    }

    *out_hdr = hdr;
    return ERROR_SUCCESS;
}

static DWORD validate_scm_handle(SC_RPC_HANDLE handle, DWORD needed_access, struct sc_manager_handle **manager)
{
    struct sc_handle *hdr;
    DWORD err = validate_context_handle(handle, SC_HTYPE_MANAGER, needed_access, &hdr);
    if (err == ERROR_SUCCESS)
        *manager = (struct sc_manager_handle *)hdr;
    return err;
}

static DWORD validate_service_handle(SC_RPC_HANDLE handle, DWORD needed_access, struct sc_service_handle **service)
{
    struct sc_handle *hdr;
    DWORD err = validate_context_handle(handle, SC_HTYPE_SERVICE, needed_access, &hdr);
    if (err == ERROR_SUCCESS)
        *service = (struct sc_service_handle *)hdr;
    return err;
}

DWORD __cdecl svcctl_OpenSCManagerW(
    MACHINE_HANDLEW MachineName, /* Note: this parameter is ignored */
    LPCWSTR DatabaseName,
    DWORD dwAccessMask,
    SC_RPC_HANDLE *handle)
{
    struct sc_manager_handle *manager;

    WINE_TRACE("(%s, %s, %x)\n", wine_dbgstr_w(MachineName), wine_dbgstr_w(DatabaseName), dwAccessMask);

    if (DatabaseName != NULL && DatabaseName[0])
    {
        if (strcmpW(DatabaseName, SERVICES_FAILED_DATABASEW) == 0)
            return ERROR_DATABASE_DOES_NOT_EXIST;
        if (strcmpW(DatabaseName, SERVICES_ACTIVE_DATABASEW) != 0)
            return ERROR_INVALID_NAME;
    }

    if (!(manager = HeapAlloc(GetProcessHeap(), 0, sizeof(*manager))))
        return ERROR_NOT_ENOUGH_SERVER_MEMORY;

    manager->hdr.type = SC_HTYPE_MANAGER;

    if (dwAccessMask & MAXIMUM_ALLOWED)
        dwAccessMask |= SC_MANAGER_ALL_ACCESS;
    manager->hdr.access = dwAccessMask;
    RtlMapGenericMask(&manager->hdr.access, &g_scm_generic);
    manager->db = active_database;
    *handle = &manager->hdr;

    return ERROR_SUCCESS;
}

static void SC_RPC_HANDLE_destroy(SC_RPC_HANDLE handle)
{
    struct sc_handle *hdr = handle;
    switch (hdr->type)
    {
        case SC_HTYPE_MANAGER:
        {
            struct sc_manager_handle *manager = (struct sc_manager_handle *)hdr;
            HeapFree(GetProcessHeap(), 0, manager);
            break;
        }
        case SC_HTYPE_SERVICE:
        {
            struct sc_service_handle *service = (struct sc_service_handle *)hdr;
            release_service(service->service_entry);
            HeapFree(GetProcessHeap(), 0, service);
            break;
        }
        default:
            WINE_ERR("invalid handle type %d\n", hdr->type);
            RpcRaiseException(ERROR_INVALID_HANDLE);
    }
}

DWORD __cdecl svcctl_GetServiceDisplayNameW(
    SC_RPC_HANDLE hSCManager,
    LPCWSTR lpServiceName,
    WCHAR *lpBuffer,
    DWORD *cchBufSize)
{
    struct sc_manager_handle *manager;
    struct service_entry *entry;
    DWORD err;

    WINE_TRACE("(%s, %d)\n", wine_dbgstr_w(lpServiceName), *cchBufSize);

    if ((err = validate_scm_handle(hSCManager, 0, &manager)) != ERROR_SUCCESS)
        return err;

    scmdatabase_lock(manager->db);

    entry = scmdatabase_find_service(manager->db, lpServiceName);
    if (entry != NULL)
    {
        LPCWSTR name;
        int len;
        name = get_display_name(entry);
        len = strlenW(name);
        if (len <= *cchBufSize)
        {
            err = ERROR_SUCCESS;
            memcpy(lpBuffer, name, (len + 1)*sizeof(*name));
        }
        else
            err = ERROR_INSUFFICIENT_BUFFER;
        *cchBufSize = len;
    }
    else
        err = ERROR_SERVICE_DOES_NOT_EXIST;

    scmdatabase_unlock(manager->db);

    if (err != ERROR_SUCCESS)
        lpBuffer[0] = 0;

    return err;
}

DWORD __cdecl svcctl_GetServiceKeyNameW(
    SC_RPC_HANDLE hSCManager,
    LPCWSTR lpServiceDisplayName,
    WCHAR *lpBuffer,
    DWORD *cchBufSize)
{
    struct service_entry *entry;
    struct sc_manager_handle *manager;
    DWORD err;

    WINE_TRACE("(%s, %d)\n", wine_dbgstr_w(lpServiceDisplayName), *cchBufSize);

    if ((err = validate_scm_handle(hSCManager, 0, &manager)) != ERROR_SUCCESS)
        return err;

    scmdatabase_lock(manager->db);

    entry = scmdatabase_find_service_by_displayname(manager->db, lpServiceDisplayName);
    if (entry != NULL)
    {
        int len;
        len = strlenW(entry->name);
        if (len <= *cchBufSize)
        {
            err = ERROR_SUCCESS;
            memcpy(lpBuffer, entry->name, (len + 1)*sizeof(*entry->name));
        }
        else
            err = ERROR_INSUFFICIENT_BUFFER;
        *cchBufSize = len;
    }
    else
        err = ERROR_SERVICE_DOES_NOT_EXIST;

    scmdatabase_unlock(manager->db);

    if (err != ERROR_SUCCESS)
        lpBuffer[0] = 0;

    return err;
}

static DWORD create_handle_for_service(struct service_entry *entry, DWORD dwDesiredAccess, SC_RPC_HANDLE *phService)
{
    struct sc_service_handle *service;

    if (!(service = HeapAlloc(GetProcessHeap(), 0, sizeof(*service))))
    {
        release_service(entry);
        return ERROR_NOT_ENOUGH_SERVER_MEMORY;
    }

    if (dwDesiredAccess & MAXIMUM_ALLOWED)
        dwDesiredAccess |= SERVICE_ALL_ACCESS;

    service->hdr.type = SC_HTYPE_SERVICE;
    service->hdr.access = dwDesiredAccess;
    RtlMapGenericMask(&service->hdr.access, &g_svc_generic);
    service->service_entry = entry;

    *phService = &service->hdr;
    return ERROR_SUCCESS;
}

DWORD __cdecl svcctl_OpenServiceW(
    SC_RPC_HANDLE hSCManager,
    LPCWSTR lpServiceName,
    DWORD dwDesiredAccess,
    SC_RPC_HANDLE *phService)
{
    struct sc_manager_handle *manager;
    struct service_entry *entry;
    DWORD err;

    WINE_TRACE("(%s, 0x%x)\n", wine_dbgstr_w(lpServiceName), dwDesiredAccess);

    if ((err = validate_scm_handle(hSCManager, 0, &manager)) != ERROR_SUCCESS)
        return err;
    if (!validate_service_name(lpServiceName))
        return ERROR_INVALID_NAME;

    scmdatabase_lock(manager->db);
    entry = scmdatabase_find_service(manager->db, lpServiceName);
    if (entry != NULL)
        InterlockedIncrement(&entry->ref_count);
    scmdatabase_unlock(manager->db);

    if (entry == NULL)
        return ERROR_SERVICE_DOES_NOT_EXIST;

    return create_handle_for_service(entry, dwDesiredAccess, phService);
}

static DWORD parse_dependencies(const WCHAR *dependencies, struct service_entry *entry)
{
    WCHAR *services = NULL, *groups, *s;
    DWORD len, len_services = 0, len_groups = 0;
    const WCHAR *ptr = dependencies;

    if (!dependencies || !dependencies[0])
    {
        entry->dependOnServices = NULL;
        entry->dependOnGroups = NULL;
        return ERROR_SUCCESS;
    }

    while (*ptr)
    {
        len = strlenW(ptr) + 1;
        if (ptr[0] == '+' && ptr[1])
            len_groups += len - 1;
        else
            len_services += len;
        ptr += len;
    }
    if (!len_services) entry->dependOnServices = NULL;
    else
    {
        services = HeapAlloc(GetProcessHeap(), 0, (len_services + 1) * sizeof(WCHAR));
        if (!services)
            return ERROR_OUTOFMEMORY;

        s = services;
        ptr = dependencies;
        while (*ptr)
        {
            len = strlenW(ptr) + 1;
            if (*ptr != '+')
            {
                strcpyW(s, ptr);
                s += len;
            }
            ptr += len;
        }
        *s = 0;
        entry->dependOnServices = services;
    }
    if (!len_groups) entry->dependOnGroups = NULL;
    else
    {
        groups = HeapAlloc(GetProcessHeap(), 0, (len_groups + 1) * sizeof(WCHAR));
        if (!groups)
        {
            HeapFree(GetProcessHeap(), 0, services);
            return ERROR_OUTOFMEMORY;
        }
        s = groups;
        ptr = dependencies;
        while (*ptr)
        {
            len = strlenW(ptr) + 1;
            if (ptr[0] == '+' && ptr[1])
            {
                strcpyW(s, ptr + 1);
                s += len - 1;
            }
            ptr += len;
        }
        *s = 0;
        entry->dependOnGroups = groups;
    }

    return ERROR_SUCCESS;
}

static DWORD create_serviceW(
    SC_RPC_HANDLE hSCManager,
    LPCWSTR lpServiceName,
    LPCWSTR lpDisplayName,
    DWORD dwDesiredAccess,
    DWORD dwServiceType,
    DWORD dwStartType,
    DWORD dwErrorControl,
    LPCWSTR lpBinaryPathName,
    LPCWSTR lpLoadOrderGroup,
    DWORD *lpdwTagId,
    const BYTE *lpDependencies,
    DWORD dwDependenciesSize,
    LPCWSTR lpServiceStartName,
    const BYTE *lpPassword,
    DWORD dwPasswordSize,
    SC_RPC_HANDLE *phService,
    BOOL is_wow64)
{
    struct service_entry *entry, *found;
    struct sc_manager_handle *manager;
    DWORD err;

    WINE_TRACE("(%s, %s, 0x%x, %s)\n", wine_dbgstr_w(lpServiceName), wine_dbgstr_w(lpDisplayName), dwDesiredAccess, wine_dbgstr_w(lpBinaryPathName));

    if ((err = validate_scm_handle(hSCManager, SC_MANAGER_CREATE_SERVICE, &manager)) != ERROR_SUCCESS)
        return err;

    if (!validate_service_name(lpServiceName))
        return ERROR_INVALID_NAME;
    if (!check_multisz((LPCWSTR)lpDependencies, dwDependenciesSize) || !lpServiceName[0] || !lpBinaryPathName[0])
        return ERROR_INVALID_PARAMETER;

    if (lpPassword)
        WINE_FIXME("Don't know how to add a password\n");   /* I always get ERROR_GEN_FAILURE */

    err = service_create(lpServiceName, &entry);
    if (err != ERROR_SUCCESS)
        return err;

    err = parse_dependencies((LPCWSTR)lpDependencies, entry);
    if (err != ERROR_SUCCESS) {
        free_service_entry(entry);
        return err;
    }

    entry->is_wow64 = is_wow64;
    entry->config.dwServiceType = entry->status.dwServiceType = dwServiceType;
    entry->config.dwStartType = dwStartType;
    entry->config.dwErrorControl = dwErrorControl;
    entry->config.lpBinaryPathName = strdupW(lpBinaryPathName);
    entry->config.lpLoadOrderGroup = strdupW(lpLoadOrderGroup);
    entry->config.lpServiceStartName = strdupW(lpServiceStartName);
    entry->config.lpDisplayName = strdupW(lpDisplayName);

    if (lpdwTagId)      /* TODO: In most situations a non-NULL TagId will generate an ERROR_INVALID_PARAMETER. */
        entry->config.dwTagId = *lpdwTagId;
    else
        entry->config.dwTagId = 0;

    /* other fields NULL*/

    if (!validate_service_config(entry))
    {
        WINE_ERR("Invalid data while trying to create service\n");
        free_service_entry(entry);
        return ERROR_INVALID_PARAMETER;
    }

    scmdatabase_lock(manager->db);

    if ((found = scmdatabase_find_service(manager->db, lpServiceName)))
    {
        err = is_marked_for_delete(found) ? ERROR_SERVICE_MARKED_FOR_DELETE : ERROR_SERVICE_EXISTS;
        scmdatabase_unlock(manager->db);
        free_service_entry(entry);
        return err;
    }

    if (scmdatabase_find_service_by_displayname(manager->db, get_display_name(entry)))
    {
        scmdatabase_unlock(manager->db);
        free_service_entry(entry);
        return ERROR_DUPLICATE_SERVICE_NAME;
    }

    err = scmdatabase_add_service(manager->db, entry);
    if (err != ERROR_SUCCESS)
    {
        scmdatabase_unlock(manager->db);
        free_service_entry(entry);
        return err;
    }
    scmdatabase_unlock(manager->db);

    return create_handle_for_service(entry, dwDesiredAccess, phService);
}

DWORD __cdecl svcctl_CreateServiceW(
    SC_RPC_HANDLE hSCManager,
    LPCWSTR lpServiceName,
    LPCWSTR lpDisplayName,
    DWORD dwDesiredAccess,
    DWORD dwServiceType,
    DWORD dwStartType,
    DWORD dwErrorControl,
    LPCWSTR lpBinaryPathName,
    LPCWSTR lpLoadOrderGroup,
    DWORD *lpdwTagId,
    const BYTE *lpDependencies,
    DWORD dwDependenciesSize,
    LPCWSTR lpServiceStartName,
    const BYTE *lpPassword,
    DWORD dwPasswordSize,
    SC_RPC_HANDLE *phService)
{
    WINE_TRACE("(%s, %s, 0x%x, %s)\n", wine_dbgstr_w(lpServiceName), wine_dbgstr_w(lpDisplayName), dwDesiredAccess, wine_dbgstr_w(lpBinaryPathName));
    return create_serviceW(hSCManager, lpServiceName, lpDisplayName, dwDesiredAccess, dwServiceType, dwStartType,
        dwErrorControl, lpBinaryPathName, lpLoadOrderGroup, lpdwTagId, lpDependencies, dwDependenciesSize, lpServiceStartName,
        lpPassword, dwPasswordSize, phService, FALSE);
}

DWORD __cdecl svcctl_DeleteService(
    SC_RPC_HANDLE hService)
{
    struct sc_service_handle *service;
    DWORD err;

    if ((err = validate_service_handle(hService, DELETE, &service)) != ERROR_SUCCESS)
        return err;

    service_lock(service->service_entry);

    if (!is_marked_for_delete(service->service_entry))
        err = mark_for_delete(service->service_entry);
    else
        err = ERROR_SERVICE_MARKED_FOR_DELETE;

    service_unlock(service->service_entry);

    return err;
}

DWORD __cdecl svcctl_QueryServiceConfigW(
        SC_RPC_HANDLE hService,
        QUERY_SERVICE_CONFIGW *config,
        DWORD buf_size,
        DWORD *needed_size)
{
    struct sc_service_handle *service;
    DWORD err;

    WINE_TRACE("(%p)\n", config);

    if ((err = validate_service_handle(hService, SERVICE_QUERY_CONFIG, &service)) != 0)
        return err;

    service_lock(service->service_entry);
    config->dwServiceType = service->service_entry->config.dwServiceType;
    config->dwStartType = service->service_entry->config.dwStartType;
    config->dwErrorControl = service->service_entry->config.dwErrorControl;
    config->lpBinaryPathName = strdupW(service->service_entry->config.lpBinaryPathName);
    config->lpLoadOrderGroup = strdupW(service->service_entry->config.lpLoadOrderGroup);
    config->dwTagId = service->service_entry->config.dwTagId;
    config->lpDependencies = NULL; /* TODO */
    config->lpServiceStartName = strdupW(service->service_entry->config.lpServiceStartName);
    config->lpDisplayName = strdupW(service->service_entry->config.lpDisplayName);
    service_unlock(service->service_entry);

    return ERROR_SUCCESS;
}

DWORD __cdecl svcctl_ChangeServiceConfigW(
        SC_RPC_HANDLE hService,
        DWORD dwServiceType,
        DWORD dwStartType,
        DWORD dwErrorControl,
        LPCWSTR lpBinaryPathName,
        LPCWSTR lpLoadOrderGroup,
        DWORD *lpdwTagId,
        const BYTE *lpDependencies,
        DWORD dwDependenciesSize,
        LPCWSTR lpServiceStartName,
        const BYTE *lpPassword,
        DWORD dwPasswordSize,
        LPCWSTR lpDisplayName)
{
    struct service_entry new_entry, *entry;
    struct sc_service_handle *service;
    DWORD err;

    WINE_TRACE("\n");

    if ((err = validate_service_handle(hService, SERVICE_CHANGE_CONFIG, &service)) != 0)
        return err;

    if (!check_multisz((LPCWSTR)lpDependencies, dwDependenciesSize))
        return ERROR_INVALID_PARAMETER;

    /* first check if the new configuration is correct */
    service_lock(service->service_entry);

    if (is_marked_for_delete(service->service_entry))
    {
        service_unlock(service->service_entry);
        return ERROR_SERVICE_MARKED_FOR_DELETE;
    }

    if (lpDisplayName != NULL &&
        (entry = scmdatabase_find_service_by_displayname(service->service_entry->db, lpDisplayName)) &&
        (entry != service->service_entry))
    {
        service_unlock(service->service_entry);
        return ERROR_DUPLICATE_SERVICE_NAME;
    }

    new_entry = *service->service_entry;

    if (dwServiceType != SERVICE_NO_CHANGE)
        new_entry.config.dwServiceType = dwServiceType;

    if (dwStartType != SERVICE_NO_CHANGE)
        new_entry.config.dwStartType = dwStartType;

    if (dwErrorControl != SERVICE_NO_CHANGE)
        new_entry.config.dwErrorControl = dwErrorControl;

    if (lpBinaryPathName != NULL)
        new_entry.config.lpBinaryPathName = (LPWSTR)lpBinaryPathName;

    if (lpLoadOrderGroup != NULL)
        new_entry.config.lpLoadOrderGroup = (LPWSTR)lpLoadOrderGroup;

    if (lpdwTagId != NULL)
        WINE_FIXME("Changing tag id not supported\n");

    if (lpServiceStartName != NULL)
        new_entry.config.lpServiceStartName = (LPWSTR)lpServiceStartName;

    if (lpPassword != NULL)
        WINE_FIXME("Setting password not supported\n");

    if (lpDisplayName != NULL)
        new_entry.config.lpDisplayName = (LPWSTR)lpDisplayName;

    err = parse_dependencies((LPCWSTR)lpDependencies, &new_entry);
    if (err != ERROR_SUCCESS)
    {
        service_unlock(service->service_entry);
        return err;
    }

    if (!validate_service_config(&new_entry))
    {
        WINE_ERR("The configuration after the change wouldn't be valid\n");
        service_unlock(service->service_entry);
        return ERROR_INVALID_PARAMETER;
    }

    /* configuration OK. The strings needs to be duplicated */
    if (lpBinaryPathName != NULL)
        new_entry.config.lpBinaryPathName = strdupW(lpBinaryPathName);

    if (lpLoadOrderGroup != NULL)
        new_entry.config.lpLoadOrderGroup = strdupW(lpLoadOrderGroup);

    if (lpServiceStartName != NULL)
        new_entry.config.lpServiceStartName = strdupW(lpServiceStartName);

    if (lpDisplayName != NULL)
        new_entry.config.lpDisplayName = strdupW(lpDisplayName);

    /* try to save to Registry, commit or rollback depending on success */
    err = save_service_config(&new_entry);
    if (ERROR_SUCCESS == err)
    {
        free_service_strings(service->service_entry, &new_entry);
        *service->service_entry = new_entry;
    }
    else free_service_strings(&new_entry, service->service_entry);
    service_unlock(service->service_entry);

    return err;
}

DWORD __cdecl svcctl_SetServiceStatus(
    SC_RPC_HANDLE hServiceStatus,
    LPSERVICE_STATUS lpServiceStatus)
{
    struct sc_service_handle *service;
    struct process_entry *process;
    DWORD err;

    WINE_TRACE("(%p, %p)\n", hServiceStatus, lpServiceStatus);

    if ((err = validate_service_handle(hServiceStatus, SERVICE_SET_STATUS, &service)) != 0)
        return err;

    service_lock(service->service_entry);
    /* FIXME: be a bit more discriminant about what parts of the status we set
     * and check that fields are valid */
    service->service_entry->status = *lpServiceStatus;
    if ((process = service->service_entry->process))
    {
        if (lpServiceStatus->dwCurrentState == SERVICE_STOPPED)
        {
            service->service_entry->process = NULL;
            terminate_after_timeout(process, service_kill_timeout);
            release_process(process);
        }
        else
            SetEvent(process->status_changed_event);
    }
    service_unlock(service->service_entry);

    return ERROR_SUCCESS;
}

DWORD __cdecl svcctl_ChangeServiceConfig2W( SC_RPC_HANDLE hService, SC_RPC_CONFIG_INFOW config )
{
    struct sc_service_handle *service;
    DWORD err;

    if ((err = validate_service_handle(hService, SERVICE_CHANGE_CONFIG, &service)) != 0)
        return err;

    switch (config.dwInfoLevel)
    {
    case SERVICE_CONFIG_DESCRIPTION:
        {
            WCHAR *descr = NULL;

            if (config.u.descr->lpDescription[0])
            {
                if (!(descr = strdupW( config.u.descr->lpDescription )))
                    return ERROR_NOT_ENOUGH_MEMORY;
            }

            WINE_TRACE( "changing service %p descr to %s\n", service, wine_dbgstr_w(descr) );
            service_lock( service->service_entry );
            HeapFree( GetProcessHeap(), 0, service->service_entry->description );
            service->service_entry->description = descr;
            save_service_config( service->service_entry );
            service_unlock( service->service_entry );
        }
        break;
    case SERVICE_CONFIG_FAILURE_ACTIONS:
        WINE_FIXME( "SERVICE_CONFIG_FAILURE_ACTIONS not implemented: period %u msg %s cmd %s\n",
                    config.u.actions->dwResetPeriod,
                    wine_dbgstr_w(config.u.actions->lpRebootMsg),
                    wine_dbgstr_w(config.u.actions->lpCommand) );
        break;
    case SERVICE_CONFIG_PRESHUTDOWN_INFO:
        WINE_TRACE( "changing service %p preshutdown timeout to %d\n",
                service, config.u.preshutdown->dwPreshutdownTimeout );
        service_lock( service->service_entry );
        service->service_entry->preshutdown_timeout = config.u.preshutdown->dwPreshutdownTimeout;
        save_service_config( service->service_entry );
        service_unlock( service->service_entry );
        break;
    default:
        WINE_FIXME("level %u not implemented\n", config.dwInfoLevel);
        err = ERROR_INVALID_LEVEL;
        break;
    }
    return err;
}

DWORD __cdecl svcctl_QueryServiceConfig2W( SC_RPC_HANDLE hService, DWORD level,
                                           BYTE *buffer, DWORD size, LPDWORD needed )
{
    struct sc_service_handle *service;
    DWORD err;

    memset(buffer, 0, size);

    if ((err = validate_service_handle(hService, SERVICE_QUERY_STATUS, &service)) != 0)
        return err;

    switch (level)
    {
    case SERVICE_CONFIG_DESCRIPTION:
        {
            SERVICE_DESCRIPTIONW *descr = (SERVICE_DESCRIPTIONW *)buffer;

            service_lock(service->service_entry);
            *needed = sizeof(*descr);
            if (service->service_entry->description)
                *needed += (strlenW(service->service_entry->description) + 1) * sizeof(WCHAR);
            if (size >= *needed)
            {
                if (service->service_entry->description)
                {
                    /* store a buffer offset instead of a pointer */
                    descr->lpDescription = (WCHAR *)((BYTE *)(descr + 1) - buffer);
                    strcpyW( (WCHAR *)(descr + 1), service->service_entry->description );
                }
                else descr->lpDescription = NULL;
            }
            else err = ERROR_INSUFFICIENT_BUFFER;
            service_unlock(service->service_entry);
        }
        break;

    case SERVICE_CONFIG_PRESHUTDOWN_INFO:
        service_lock(service->service_entry);

        *needed = sizeof(SERVICE_PRESHUTDOWN_INFO);
        if (size >= *needed)
            ((LPSERVICE_PRESHUTDOWN_INFO)buffer)->dwPreshutdownTimeout =
                service->service_entry->preshutdown_timeout;
        else err = ERROR_INSUFFICIENT_BUFFER;

        service_unlock(service->service_entry);
        break;

    default:
        WINE_FIXME("level %u not implemented\n", level);
        err = ERROR_INVALID_LEVEL;
        break;
    }
    return err;
}

static void fill_status_process(SERVICE_STATUS_PROCESS *status, struct service_entry *service)
{
    struct process_entry *process = service->process;
    memcpy(status, &service->status, sizeof(service->status));
    status->dwProcessId     = process ? process->process_id : 0;
    status->dwServiceFlags  = 0;
}

DWORD __cdecl svcctl_QueryServiceStatusEx(
    SC_RPC_HANDLE hService,
    SC_STATUS_TYPE InfoLevel,
    BYTE *lpBuffer,
    DWORD cbBufSize,
    LPDWORD pcbBytesNeeded)
{
    struct sc_service_handle *service;
    DWORD err;
    LPSERVICE_STATUS_PROCESS pSvcStatusData;

    memset(lpBuffer, 0, cbBufSize);

    if ((err = validate_service_handle(hService, SERVICE_QUERY_STATUS, &service)) != 0)
        return err;

    if (InfoLevel != SC_STATUS_PROCESS_INFO)
        return ERROR_INVALID_LEVEL;

    pSvcStatusData = (LPSERVICE_STATUS_PROCESS) lpBuffer;
    if (pSvcStatusData == NULL)
        return ERROR_INVALID_PARAMETER;

    if (cbBufSize < sizeof(SERVICE_STATUS_PROCESS))
    {
        if( pcbBytesNeeded != NULL)
            *pcbBytesNeeded = sizeof(SERVICE_STATUS_PROCESS);

        return ERROR_INSUFFICIENT_BUFFER;
    }

    service_lock(service->service_entry);
    fill_status_process(pSvcStatusData, service->service_entry);
    service_unlock(service->service_entry);

    return ERROR_SUCCESS;
}

/******************************************************************************
 * service_accepts_control
 */
static BOOL service_accepts_control(const struct service_entry *service, DWORD dwControl)
{
    DWORD a = service->status.dwControlsAccepted;

    if (dwControl >= 128 && dwControl <= 255)
        return TRUE;

    switch (dwControl)
    {
    case SERVICE_CONTROL_INTERROGATE:
        return TRUE;
    case SERVICE_CONTROL_STOP:
        if (a&SERVICE_ACCEPT_STOP)
            return TRUE;
        break;
    case SERVICE_CONTROL_SHUTDOWN:
        if (a&SERVICE_ACCEPT_SHUTDOWN)
            return TRUE;
        break;
    case SERVICE_CONTROL_PAUSE:
    case SERVICE_CONTROL_CONTINUE:
        if (a&SERVICE_ACCEPT_PAUSE_CONTINUE)
            return TRUE;
        break;
    case SERVICE_CONTROL_PARAMCHANGE:
        if (a&SERVICE_ACCEPT_PARAMCHANGE)
            return TRUE;
        break;
    case SERVICE_CONTROL_NETBINDADD:
    case SERVICE_CONTROL_NETBINDREMOVE:
    case SERVICE_CONTROL_NETBINDENABLE:
    case SERVICE_CONTROL_NETBINDDISABLE:
        if (a&SERVICE_ACCEPT_NETBINDCHANGE)
            return TRUE;
    case SERVICE_CONTROL_HARDWAREPROFILECHANGE:
        if (a&SERVICE_ACCEPT_HARDWAREPROFILECHANGE)
            return TRUE;
        break;
    case SERVICE_CONTROL_POWEREVENT:
        if (a&SERVICE_ACCEPT_POWEREVENT)
            return TRUE;
        break;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (a&SERVICE_ACCEPT_SESSIONCHANGE)
            return TRUE;
        break;
    }
    return FALSE;
}

/******************************************************************************
 * process_send_command
 */
BOOL process_send_command(struct process_entry *process, const void *data, DWORD size, DWORD *result)
{
    OVERLAPPED overlapped;
    DWORD count, ret;
    BOOL r;

    overlapped.hEvent = process->overlapped_event;
    r = WriteFile(process->control_pipe, data, size, &count, &overlapped);
    if (!r && GetLastError() == ERROR_IO_PENDING)
    {
        ret = WaitForSingleObject(process->overlapped_event, service_pipe_timeout);
        if (ret == WAIT_TIMEOUT)
        {
            WINE_ERR("sending command timed out\n");
            *result = ERROR_SERVICE_REQUEST_TIMEOUT;
            return FALSE;
        }
        r = GetOverlappedResult(process->control_pipe, &overlapped, &count, FALSE);
    }
    if (!r || count != size)
    {
        WINE_ERR("service protocol error - failed to write pipe!\n");
        *result  = (!r ? GetLastError() : ERROR_WRITE_FAULT);
        return FALSE;
    }
    r = ReadFile(process->control_pipe, result, sizeof *result, &count, &overlapped);
    if (!r && GetLastError() == ERROR_IO_PENDING)
    {
        ret = WaitForSingleObject(process->overlapped_event, service_pipe_timeout);
        if (ret == WAIT_TIMEOUT)
        {
            WINE_ERR("receiving command result timed out\n");
            *result = ERROR_SERVICE_REQUEST_TIMEOUT;
            return FALSE;
        }
        r = GetOverlappedResult(process->control_pipe, &overlapped, &count, FALSE);
    }
    if (!r || count != sizeof *result)
    {
        WINE_ERR("service protocol error - failed to read pipe "
            "r = %d  count = %d!\n", r, count);
        *result = (!r ? GetLastError() : ERROR_READ_FAULT);
        return FALSE;
    }

    return TRUE;
}

/******************************************************************************
 * process_send_control
 */
static BOOL process_send_control(struct process_entry *process, const WCHAR *name, DWORD control,
                                 const BYTE *data, DWORD data_size, DWORD *result)
{
    service_start_info *ssi;
    DWORD len;
    BOOL r;

    /* calculate how much space we need to send the startup info */
    len = (strlenW(name) + 1) * sizeof(WCHAR) + data_size;

    ssi = HeapAlloc(GetProcessHeap(),0,FIELD_OFFSET(service_start_info, data[len]));
    ssi->cmd = WINESERV_SENDCONTROL;
    ssi->control = control;
    ssi->total_size = FIELD_OFFSET(service_start_info, data[len]);
    ssi->name_size = strlenW(name) + 1;
    strcpyW((WCHAR *)ssi->data, name);
    if (data_size) memcpy(&ssi->data[ssi->name_size * sizeof(WCHAR)], data, data_size);

    r = process_send_command(process, ssi, ssi->total_size, result);
    HeapFree( GetProcessHeap(), 0, ssi );
    return r;
}

DWORD __cdecl svcctl_StartServiceW(
    SC_RPC_HANDLE hService,
    DWORD dwNumServiceArgs,
    LPCWSTR *lpServiceArgVectors)
{
    struct sc_service_handle *service;
    DWORD err;

    WINE_TRACE("(%p, %d, %p)\n", hService, dwNumServiceArgs, lpServiceArgVectors);

    if ((err = validate_service_handle(hService, SERVICE_START, &service)) != 0)
        return err;

    if (service->service_entry->config.dwStartType == SERVICE_DISABLED)
        return ERROR_SERVICE_DISABLED;

    err = service_start(service->service_entry, dwNumServiceArgs, lpServiceArgVectors);

    return err;
}

DWORD __cdecl svcctl_ControlService(
    SC_RPC_HANDLE hService,
    DWORD dwControl,
    SERVICE_STATUS *lpServiceStatus)
{
    DWORD access_required;
    struct sc_service_handle *service;
    struct process_entry *process;
    DWORD result;
    BOOL ret;

    WINE_TRACE("(%p, %d, %p)\n", hService, dwControl, lpServiceStatus);

    switch (dwControl)
    {
    case SERVICE_CONTROL_CONTINUE:
    case SERVICE_CONTROL_NETBINDADD:
    case SERVICE_CONTROL_NETBINDDISABLE:
    case SERVICE_CONTROL_NETBINDENABLE:
    case SERVICE_CONTROL_NETBINDREMOVE:
    case SERVICE_CONTROL_PARAMCHANGE:
    case SERVICE_CONTROL_PAUSE:
        access_required = SERVICE_PAUSE_CONTINUE;
        break;
    case SERVICE_CONTROL_INTERROGATE:
        access_required = SERVICE_INTERROGATE;
        break;
    case SERVICE_CONTROL_STOP:
        access_required = SERVICE_STOP;
        break;
    default:
        if (dwControl >= 128 && dwControl <= 255)
            access_required = SERVICE_USER_DEFINED_CONTROL;
        else
            return ERROR_INVALID_PARAMETER;
    }

    if ((result = validate_service_handle(hService, access_required, &service)) != 0)
        return result;

    service_lock(service->service_entry);

    result = ERROR_SUCCESS;
    switch (service->service_entry->status.dwCurrentState)
    {
    case SERVICE_STOPPED:
        result = ERROR_SERVICE_NOT_ACTIVE;
        break;
    case SERVICE_START_PENDING:
        if (dwControl==SERVICE_CONTROL_STOP)
            break;
        /* fall through */
    case SERVICE_STOP_PENDING:
        result = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
        break;
    }

    if (result == ERROR_SUCCESS && service->service_entry->force_shutdown)
    {
        result = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
        if ((process = service->service_entry->process))
            process_terminate(process);
    }

    if (result != ERROR_SUCCESS)
    {
        if (lpServiceStatus) *lpServiceStatus = service->service_entry->status;
        service_unlock(service->service_entry);
        return result;
    }

    if (!service_accepts_control(service->service_entry, dwControl))
    {
        service_unlock(service->service_entry);
        return ERROR_INVALID_SERVICE_CONTROL;
    }

    /* Remember that we tried to shutdown this service. When the service is
     * still running on the second invocation, it will be forcefully killed. */
    if (dwControl == SERVICE_CONTROL_STOP)
        service->service_entry->force_shutdown = TRUE;

    /* Hold a reference to the process while sending the command. */
    process = grab_process(service->service_entry->process);
    service_unlock(service->service_entry);

    if (!process)
        return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;

    ret = WaitForSingleObject(process->control_mutex, 30000);
    if (ret != WAIT_OBJECT_0)
    {
        release_process(process);
        return ERROR_SERVICE_REQUEST_TIMEOUT;
    }

    if (process_send_control(process, service->service_entry->name, dwControl, NULL, 0, &result))
        result = ERROR_SUCCESS;

    if (lpServiceStatus)
    {
        service_lock(service->service_entry);
        *lpServiceStatus = service->service_entry->status;
        service_unlock(service->service_entry);
    }

    ReleaseMutex(process->control_mutex);
    release_process(process);
    return result;
}

DWORD __cdecl svcctl_CloseServiceHandle(
    SC_RPC_HANDLE *handle)
{
    WINE_TRACE("(&%p)\n", *handle);

    SC_RPC_HANDLE_destroy(*handle);
    *handle = NULL;

    return ERROR_SUCCESS;
}

static void SC_RPC_LOCK_destroy(SC_RPC_LOCK hLock)
{
    struct sc_lock *lock = hLock;
    scmdatabase_unlock_startup(lock->db);
    HeapFree(GetProcessHeap(), 0, lock);
}

void __RPC_USER SC_RPC_LOCK_rundown(SC_RPC_LOCK hLock)
{
    SC_RPC_LOCK_destroy(hLock);
}

DWORD __cdecl svcctl_LockServiceDatabase(
    SC_RPC_HANDLE hSCManager,
    SC_RPC_LOCK *phLock)
{
    struct sc_manager_handle *manager;
    struct sc_lock *lock;
    DWORD err;

    WINE_TRACE("(%p, %p)\n", hSCManager, phLock);

    if ((err = validate_scm_handle(hSCManager, SC_MANAGER_LOCK, &manager)) != ERROR_SUCCESS)
        return err;

    err = scmdatabase_lock_startup(manager->db);
    if (err != ERROR_SUCCESS)
        return err;

    lock = HeapAlloc(GetProcessHeap(), 0, sizeof(struct sc_lock));
    if (!lock)
    {
        scmdatabase_unlock_startup(manager->db);
        return ERROR_NOT_ENOUGH_SERVER_MEMORY;
    }

    lock->db = manager->db;
    *phLock = lock;

    return ERROR_SUCCESS;
}

DWORD __cdecl svcctl_UnlockServiceDatabase(
    SC_RPC_LOCK *phLock)
{
    WINE_TRACE("(&%p)\n", *phLock);

    SC_RPC_LOCK_destroy(*phLock);
    *phLock = NULL;

    return ERROR_SUCCESS;
}

static BOOL map_state(DWORD state, DWORD mask)
{
    switch (state)
    {
    case SERVICE_START_PENDING:
    case SERVICE_STOP_PENDING:
    case SERVICE_RUNNING:
    case SERVICE_CONTINUE_PENDING:
    case SERVICE_PAUSE_PENDING:
    case SERVICE_PAUSED:
        if (SERVICE_ACTIVE & mask) return TRUE;
        break;
    case SERVICE_STOPPED:
        if (SERVICE_INACTIVE & mask) return TRUE;
        break;
    default:
        WINE_ERR("unknown state %u\n", state);
        break;
    }
    return FALSE;
}

DWORD __cdecl svcctl_EnumServicesStatusW(
    SC_RPC_HANDLE hmngr,
    DWORD type,
    DWORD state,
    BYTE *buffer,
    DWORD size,
    LPDWORD needed,
    LPDWORD returned,
    LPDWORD resume)
{
    DWORD err, sz, total_size, num_services;
    DWORD_PTR offset;
    struct sc_manager_handle *manager;
    struct service_entry *service;
    ENUM_SERVICE_STATUSW *s;

    WINE_TRACE("(%p, 0x%x, 0x%x, %p, %u, %p, %p, %p)\n", hmngr, type, state, buffer, size, needed, returned, resume);

    if (!type || !state)
        return ERROR_INVALID_PARAMETER;

    if ((err = validate_scm_handle(hmngr, SC_MANAGER_ENUMERATE_SERVICE, &manager)) != ERROR_SUCCESS)
        return err;

    if (resume)
        WINE_FIXME("resume index not supported\n");

    scmdatabase_lock(manager->db);

    total_size = num_services = 0;
    LIST_FOR_EACH_ENTRY(service, &manager->db->services, struct service_entry, entry)
    {
        if ((service->status.dwServiceType & type) && map_state(service->status.dwCurrentState, state))
        {
            total_size += sizeof(ENUM_SERVICE_STATUSW);
            total_size += (strlenW(service->name) + 1) * sizeof(WCHAR);
            if (service->config.lpDisplayName)
            {
                total_size += (strlenW(service->config.lpDisplayName) + 1) * sizeof(WCHAR);
            }
            num_services++;
        }
    }
    *returned = 0;
    *needed = total_size;
    if (total_size > size)
    {
        scmdatabase_unlock(manager->db);
        return ERROR_MORE_DATA;
    }
    s = (ENUM_SERVICE_STATUSW *)buffer;
    offset = num_services * sizeof(ENUM_SERVICE_STATUSW);
    LIST_FOR_EACH_ENTRY(service, &manager->db->services, struct service_entry, entry)
    {
        if ((service->status.dwServiceType & type) && map_state(service->status.dwCurrentState, state))
        {
            sz = (strlenW(service->name) + 1) * sizeof(WCHAR);
            memcpy(buffer + offset, service->name, sz);
            s->lpServiceName = (WCHAR *)offset; /* store a buffer offset instead of a pointer */
            offset += sz;

            if (!service->config.lpDisplayName) s->lpDisplayName = NULL;
            else
            {
                sz = (strlenW(service->config.lpDisplayName) + 1) * sizeof(WCHAR);
                memcpy(buffer + offset, service->config.lpDisplayName, sz);
                s->lpDisplayName = (WCHAR *)offset;
                offset += sz;
            }
            s->ServiceStatus = service->status;
            s++;
        }
    }
    *returned = num_services;
    *needed = 0;
    scmdatabase_unlock(manager->db);
    return ERROR_SUCCESS;
}

static struct service_entry *find_service_by_group(struct scmdatabase *db, const WCHAR *group)
{
    struct service_entry *service;
    LIST_FOR_EACH_ENTRY(service, &db->services, struct service_entry, entry)
    {
        if (service->config.lpLoadOrderGroup && !strcmpiW(group, service->config.lpLoadOrderGroup))
            return service;
    }
    return NULL;
}

static BOOL match_group(const WCHAR *g1, const WCHAR *g2)
{
    if (!g2) return TRUE;
    if (!g2[0] && (!g1 || !g1[0])) return TRUE;
    if (g1 && !strcmpW(g1, g2)) return TRUE;
    return FALSE;
}

DWORD __cdecl svcctl_EnumServicesStatusExA(
    SC_RPC_HANDLE scmanager,
    SC_ENUM_TYPE info_level,
    DWORD service_type,
    DWORD service_state,
    BYTE *buffer,
    DWORD buf_size,
    DWORD *needed_size,
    DWORD *services_count,
    DWORD *resume_index,
    LPCSTR groupname)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_EnumServicesStatusExW(
    SC_RPC_HANDLE hmngr,
    SC_ENUM_TYPE info_level,
    DWORD type,
    DWORD state,
    BYTE *buffer,
    DWORD size,
    LPDWORD needed,
    LPDWORD returned,
    DWORD *resume_handle,
    LPCWSTR group)
{
    DWORD err, sz, total_size, num_services;
    DWORD_PTR offset;
    struct sc_manager_handle *manager;
    struct service_entry *service;
    ENUM_SERVICE_STATUS_PROCESSW *s;

    WINE_TRACE("(%p, 0x%x, 0x%x, %p, %u, %p, %p, %s)\n", hmngr, type, state, buffer, size,
               needed, returned, wine_dbgstr_w(group));

    if (resume_handle)
        FIXME("resume handle not supported\n");

    if (!type || !state)
        return ERROR_INVALID_PARAMETER;

    if ((err = validate_scm_handle(hmngr, SC_MANAGER_ENUMERATE_SERVICE, &manager)) != ERROR_SUCCESS)
        return err;

    scmdatabase_lock(manager->db);

    if (group && !find_service_by_group(manager->db, group))
    {
        scmdatabase_unlock(manager->db);
        return ERROR_SERVICE_DOES_NOT_EXIST;
    }

    total_size = num_services = 0;
    LIST_FOR_EACH_ENTRY(service, &manager->db->services, struct service_entry, entry)
    {
        if ((service->status.dwServiceType & type) && map_state(service->status.dwCurrentState, state)
            && match_group(service->config.lpLoadOrderGroup, group))
        {
            total_size += sizeof(ENUM_SERVICE_STATUS_PROCESSW);
            total_size += (strlenW(service->name) + 1) * sizeof(WCHAR);
            if (service->config.lpDisplayName)
            {
                total_size += (strlenW(service->config.lpDisplayName) + 1) * sizeof(WCHAR);
            }
            num_services++;
        }
    }
    *returned = 0;
    *needed = total_size;
    if (total_size > size)
    {
        scmdatabase_unlock(manager->db);
        return ERROR_MORE_DATA;
    }
    s = (ENUM_SERVICE_STATUS_PROCESSW *)buffer;
    offset = num_services * sizeof(ENUM_SERVICE_STATUS_PROCESSW);
    LIST_FOR_EACH_ENTRY(service, &manager->db->services, struct service_entry, entry)
    {
        if ((service->status.dwServiceType & type) && map_state(service->status.dwCurrentState, state)
            && match_group(service->config.lpLoadOrderGroup, group))
        {
            sz = (strlenW(service->name) + 1) * sizeof(WCHAR);
            memcpy(buffer + offset, service->name, sz);
            s->lpServiceName = (WCHAR *)offset; /* store a buffer offset instead of a pointer */
            offset += sz;

            if (!service->config.lpDisplayName) s->lpDisplayName = NULL;
            else
            {
                sz = (strlenW(service->config.lpDisplayName) + 1) * sizeof(WCHAR);
                memcpy(buffer + offset, service->config.lpDisplayName, sz);
                s->lpDisplayName = (WCHAR *)offset;
                offset += sz;
            }
            fill_status_process(&s->ServiceStatusProcess, service);
            s++;
        }
    }
    *returned = num_services;
    *needed = 0;
    scmdatabase_unlock(manager->db);
    return ERROR_SUCCESS;
}

DWORD __cdecl svcctl_unknown43(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_CreateServiceWOW64A(
    SC_RPC_HANDLE scmanager,
    LPCSTR servicename,
    LPCSTR displayname,
    DWORD accessmask,
    DWORD service_type,
    DWORD start_type,
    DWORD error_control,
    LPCSTR imagepath,
    LPCSTR loadordergroup,
    DWORD *tagid,
    const BYTE *dependencies,
    DWORD depend_size,
    LPCSTR start_name,
    const BYTE *password,
    DWORD password_size,
    SC_RPC_HANDLE *service)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_CreateServiceWOW64W(
    SC_RPC_HANDLE scmanager,
    LPCWSTR servicename,
    LPCWSTR displayname,
    DWORD accessmask,
    DWORD service_type,
    DWORD start_type,
    DWORD error_control,
    LPCWSTR imagepath,
    LPCWSTR loadordergroup,
    DWORD *tagid,
    const BYTE *dependencies,
    DWORD depend_size,
    LPCWSTR start_name,
    const BYTE *password,
    DWORD password_size,
    SC_RPC_HANDLE *service)
{
    WINE_TRACE("(%s, %s, 0x%x, %s)\n", wine_dbgstr_w(servicename), wine_dbgstr_w(displayname), accessmask, wine_dbgstr_w(imagepath));
    return create_serviceW(scmanager, servicename, displayname, accessmask, service_type, start_type, error_control, imagepath,
        loadordergroup, tagid, dependencies, depend_size, start_name, password, password_size, service, TRUE);
}

DWORD __cdecl svcctl_unknown46(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_NotifyServiceStatusChange(
    SC_RPC_HANDLE service,
    SC_RPC_NOTIFY_PARAMS params,
    GUID *clientprocessguid,
    GUID *scmprocessguid,
    BOOL *createremotequeue,
    SC_NOTIFY_RPC_HANDLE *notify)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_GetNotifyResults(
    SC_NOTIFY_RPC_HANDLE notify,
    SC_RPC_NOTIFY_PARAMS_LIST **params)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_CloseNotifyHandle(
    SC_NOTIFY_RPC_HANDLE *notify,
    BOOL *apc_fired)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_ControlServiceExA(
    SC_RPC_HANDLE service,
    DWORD control,
    DWORD info_level,
    SC_RPC_SERVICE_CONTROL_IN_PARAMSA *in_params,
    SC_RPC_SERVICE_CONTROL_OUT_PARAMSA *out_params)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_ControlServiceExW(
    SC_RPC_HANDLE service,
    DWORD control,
    DWORD info_level,
    SC_RPC_SERVICE_CONTROL_IN_PARAMSW *in_params,
    SC_RPC_SERVICE_CONTROL_OUT_PARAMSW *out_params)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_unknown52(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_unknown53(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_unknown54(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_unknown55(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceConfigEx(
    SC_RPC_HANDLE service,
    DWORD info_level,
    SC_RPC_CONFIG_INFOW *info)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceObjectSecurity(
    SC_RPC_HANDLE service,
    SECURITY_INFORMATION info,
    BYTE *descriptor,
    DWORD buf_size,
    DWORD *needed_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_SetServiceObjectSecurity(
    SC_RPC_HANDLE service,
    SECURITY_INFORMATION info,
    BYTE *descriptor,
    DWORD buf_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceStatus(
    SC_RPC_HANDLE service,
    SERVICE_STATUS *status)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_NotifyBootConfigStatus(
    SVCCTL_HANDLEW machinename,
    DWORD boot_acceptable)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_SCSetServiceBitsW(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_EnumDependentServicesW(
    SC_RPC_HANDLE service,
    DWORD state,
    BYTE *services,
    DWORD buf_size,
    DWORD *needed_size,
    DWORD *services_ret)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceLockStatusW(
    SC_RPC_HANDLE scmanager,
    QUERY_SERVICE_LOCK_STATUSW *status,
    DWORD buf_size,
    DWORD *needed_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_SCSetServiceBitsA(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_ChangeServiceConfigA(
    SC_RPC_HANDLE service,
    DWORD service_type,
    DWORD start_type,
    DWORD error_control,
    LPSTR binarypath,
    LPSTR loadordergroup,
    DWORD *tagid,
    BYTE *dependencies,
    DWORD depend_size,
    LPSTR startname,
    BYTE *password,
    DWORD password_size,
    LPSTR displayname)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_CreateServiceA(
    SC_RPC_HANDLE scmanager,
    LPCSTR servicename,
    LPCSTR displayname,
    DWORD desiredaccess,
    DWORD service_type,
    DWORD start_type,
    DWORD error_control,
    LPCSTR binarypath,
    LPCSTR loadordergroup,
    DWORD *tagid,
    const BYTE *dependencies,
    DWORD depend_size,
    LPCSTR startname,
    const BYTE *password,
    DWORD password_size,
    SC_RPC_HANDLE *service)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_EnumDependentServicesA(
    SC_RPC_HANDLE service,
    DWORD state,
    BYTE *services,
    DWORD buf_size,
    DWORD *needed_size,
    DWORD *services_ret)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_EnumServicesStatusA(
    SC_RPC_HANDLE hmngr,
    DWORD type,
    DWORD state,
    BYTE *buffer,
    DWORD size,
    DWORD *needed,
    DWORD *returned,
    DWORD *resume)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_OpenSCManagerA(
    MACHINE_HANDLEA MachineName,
    LPCSTR DatabaseName,
    DWORD dwAccessMask,
    SC_RPC_HANDLE *handle)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_OpenServiceA(
    SC_RPC_HANDLE hSCManager,
    LPCSTR lpServiceName,
    DWORD dwDesiredAccess,
    SC_RPC_HANDLE *phService)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceConfigA(
    SC_RPC_HANDLE hService,
    QUERY_SERVICE_CONFIGA *config,
    DWORD buf_size,
    DWORD *needed_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceLockStatusA(
    SC_RPC_HANDLE scmanager,
    QUERY_SERVICE_LOCK_STATUSA *status,
    DWORD buf_size,
    DWORD *needed_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_StartServiceA(
    SC_RPC_HANDLE service,
    DWORD argc,
    LPCSTR *args)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_GetServiceDisplayNameA(
    SC_RPC_HANDLE hSCManager,
    LPCSTR servicename,
    CHAR buffer[],
    DWORD *buf_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_GetServiceKeyNameA(
    SC_RPC_HANDLE hSCManager,
    LPCSTR servicename,
    CHAR buffer[],
    DWORD *buf_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_GetCurrentGroupStateW(void)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_EnumServiceGroupW(
    SC_RPC_HANDLE scmanager,
    DWORD service_type,
    DWORD service_state,
    BYTE *buffer,
    DWORD buf_size,
    DWORD *needed_size,
    DWORD *returned_size,
    DWORD *resume_index,
    LPCWSTR groupname)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_ChangeServiceConfig2A(
    SC_RPC_HANDLE service,
    SC_RPC_CONFIG_INFOA info)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD __cdecl svcctl_QueryServiceConfig2A(
    SC_RPC_HANDLE service,
    DWORD info_level,
    BYTE *buffer,
    DWORD buf_size,
    DWORD *needed_size)
{
    WINE_FIXME("\n");
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD RPC_Init(void)
{
    WCHAR transport[] = SVCCTL_TRANSPORT;
    WCHAR endpoint[] = SVCCTL_ENDPOINT;
    DWORD err;

    if ((err = RpcServerUseProtseqEpW(transport, 0, endpoint, NULL)) != ERROR_SUCCESS)
    {
        WINE_ERR("RpcServerUseProtseq failed with error %u\n", err);
        return err;
    }

    if ((err = RpcServerRegisterIf(svcctl_v2_0_s_ifspec, 0, 0)) != ERROR_SUCCESS)
    {
        WINE_ERR("RpcServerRegisterIf failed with error %u\n", err);
        return err;
    }

    if ((err = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE)) != ERROR_SUCCESS)
    {
        WINE_ERR("RpcServerListen failed with error %u\n", err);
        return err;
    }
    return ERROR_SUCCESS;
}

DWORD events_loop(void)
{
    struct timeout_queue_elem *iter, *iter_safe;
    DWORD err;
    HANDLE wait_handles[MAXIMUM_WAIT_OBJECTS];
    DWORD timeout = INFINITE;

    wait_handles[0] = __wine_make_process_system();
    wait_handles[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    timeout_queue_event = wait_handles[1];

    SetEvent(g_hStartedEvent);

    WINE_TRACE("Entered main loop\n");

    do
    {
        DWORD num_handles = 2;

        /* monitor tracked process handles for process end */
        EnterCriticalSection(&timeout_queue_cs);
        LIST_FOR_EACH_ENTRY(iter, &timeout_queue, struct timeout_queue_elem, entry)
        {
            if(num_handles == MAXIMUM_WAIT_OBJECTS){
                WINE_TRACE("Exceeded maximum wait object count\n");
                break;
            }
            wait_handles[num_handles] = iter->process->process;
            num_handles++;
        }
        LeaveCriticalSection(&timeout_queue_cs);

        err = WaitForMultipleObjects(num_handles, wait_handles, FALSE, timeout);
        WINE_TRACE("Wait returned %d\n", err);

        if(err > WAIT_OBJECT_0 || err == WAIT_TIMEOUT)
        {
            FILETIME cur_time;
            ULARGE_INTEGER time;
            DWORD idx = 0;

            GetSystemTimeAsFileTime(&cur_time);
            time.u.LowPart = cur_time.dwLowDateTime;
            time.u.HighPart = cur_time.dwHighDateTime;

            EnterCriticalSection(&timeout_queue_cs);
            timeout = INFINITE;
            LIST_FOR_EACH_ENTRY_SAFE(iter, iter_safe, &timeout_queue, struct timeout_queue_elem, entry)
            {
                if(CompareFileTime(&cur_time, &iter->time) >= 0 ||
                        (err > WAIT_OBJECT_0 + 1 && idx == err - WAIT_OBJECT_0 - 2))
                {
                    LeaveCriticalSection(&timeout_queue_cs);
                    process_terminate(iter->process);
                    EnterCriticalSection(&timeout_queue_cs);

                    release_process(iter->process);
                    list_remove(&iter->entry);
                    HeapFree(GetProcessHeap(), 0, iter);
                }
                else
                {
                    ULARGE_INTEGER time_diff;

                    time_diff.u.LowPart = iter->time.dwLowDateTime;
                    time_diff.u.HighPart = iter->time.dwHighDateTime;
                    time_diff.QuadPart = (time_diff.QuadPart-time.QuadPart)/10000;

                    if(time_diff.QuadPart < timeout)
                        timeout = time_diff.QuadPart;
                }
                idx++;
            }
            LeaveCriticalSection(&timeout_queue_cs);

            if(timeout != INFINITE)
                timeout += 1000;
        }
    } while (err != WAIT_OBJECT_0);

    WINE_TRACE("Object signaled - wine shutdown\n");
    EnterCriticalSection(&timeout_queue_cs);
    LIST_FOR_EACH_ENTRY_SAFE(iter, iter_safe, &timeout_queue, struct timeout_queue_elem, entry)
    {
        LeaveCriticalSection(&timeout_queue_cs);
        process_terminate(iter->process);
        EnterCriticalSection(&timeout_queue_cs);

        release_process(iter->process);
        list_remove(&iter->entry);
        HeapFree(GetProcessHeap(), 0, iter);
    }
    LeaveCriticalSection(&timeout_queue_cs);

    CloseHandle(wait_handles[0]);
    CloseHandle(wait_handles[1]);
    return ERROR_SUCCESS;
}

void __RPC_USER SC_RPC_HANDLE_rundown(SC_RPC_HANDLE handle)
{
    SC_RPC_HANDLE_destroy(handle);
}

void __RPC_USER SC_NOTIFY_RPC_HANDLE_rundown(SC_NOTIFY_RPC_HANDLE handle)
{
}

void  __RPC_FAR * __RPC_USER MIDL_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), 0, len);
}

void __RPC_USER MIDL_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}
